// Copyright (c) 2014 Dropbox, Inc.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//    http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sstream>
#include <unordered_map>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"

#include "core/common.h"
#include "core/stats.h"

#include "core/util.h"

#include "codegen/codegen.h"
#include "codegen/llvm_interpreter.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/util.h"

namespace pyston {

union Val {
    bool b;
    int64_t n;
    double d;
    Box* o;

    Val(bool b) : b(b) {}
    Val(int64_t n) : n(n) {}
    Val(double d) : d(d) {}
    Val(Box* o) : o(o) {}
};

typedef std::unordered_map<llvm::Value*, Val> SymMap;

int width(llvm::Type *t, const llvm::DataLayout &dl) {
    return dl.getTypeSizeInBits(t) / 8;
    //if (t == g.i1) return 1;
    //if (t == g.i64) return 8;
    //if (t->isPointerTy()) return 8;
//
    //t->dump();
    //RELEASE_ASSERT(0, "");
}

int width(llvm::Value *v, const llvm::DataLayout &dl) {
    return width(v->getType(), dl);
}

//#undef VERBOSITY
//#define VERBOSITY(x) 2
#define TIME_INTERPRETS

Val fetch(llvm::Value* v, const llvm::DataLayout &dl, const SymMap &symbols) {
    assert(v);

    int opcode = v->getValueID();

    //std::ostringstream os("");
    //os << "fetch_" << opcode;
    //int statid = Stats::getStatId(os.str());
    //Stats::log(statid);

    if (opcode >= llvm::Value::InstructionVal) {
        assert(symbols.count(v));
        return symbols.find(v)->second;
    }

    switch(opcode) {
        case llvm::Value::ArgumentVal: {
            assert(symbols.count(v));
            return symbols.find(v)->second;
        }
        case llvm::Value::ConstantIntVal: {
            if (v->getType() == g.i1)
                return (int64_t)llvm::cast<llvm::ConstantInt>(v)->getZExtValue();
            if (v->getType() == g.i64 || v->getType() == g.i32)
                return llvm::cast<llvm::ConstantInt>(v)->getSExtValue();
            v->dump();
            RELEASE_ASSERT(0, "");
        }
        case llvm::Value::ConstantFPVal: {
            return llvm::cast<llvm::ConstantFP>(v)->getValueAPF().convertToDouble();
        }
        case llvm::Value::ConstantExprVal: {
            llvm::ConstantExpr *ce = llvm::cast<llvm::ConstantExpr>(v);
            if (ce->isCast()) {
                assert(width(ce->getOperand(0), dl) == 8 && width(ce, dl) == 8);

                Val o = fetch(ce->getOperand(0), dl, symbols);
                return o;
            } else if (ce->getOpcode() == llvm::Instruction::GetElementPtr) {
                int64_t base = (int64_t)fetch(ce->getOperand(0), dl, symbols).o;
                llvm::Type *t = ce->getOperand(0)->getType();

                llvm::User::value_op_iterator begin = ce->value_op_begin();
                ++begin;
                std::vector<llvm::Value*> indices(begin, ce->value_op_end());

                int64_t offset = dl.getIndexedOffset(t, indices);

                /*if (VERBOSITY()) {
                    ce->dump();
                    ce->getOperand(0)->dump();
                    for (int i = 0; i < indices.size() ;i++) {
                        indices[i]->dump();
                    }
                    printf("resulting offset: %ld\n", offset);
                }*/

                return base + offset;
            } else {
                v->dump();
                RELEASE_ASSERT(0, "");
            }
        }
        /*case llvm::Value::FunctionVal: {
            llvm::Function* f = llvm::cast<llvm::Function>(v);
            if (f->getName() == "printf") {
                return (int64_t)printf;
            } else if (f->getName() == "reoptCompiledFunc") {
                return (int64_t)reoptCompiledFunc;
            } else if (f->getName() == "compilePartialFunc") {
                return (int64_t)compilePartialFunc;
            } else if (startswith(f->getName(), "runtimeCall")) {
                return (int64_t)g.func_registry.getFunctionAddress("runtimeCall");
            } else {
                return (int64_t)g.func_registry.getFunctionAddress(f->getName());
            }
        }*/
        case llvm::Value::GlobalVariableVal: {
            llvm::GlobalVariable* gv = llvm::cast<llvm::GlobalVariable>(v);
            if (!gv->isDeclaration() && gv->getLinkage() == llvm::GlobalVariable::InternalLinkage) {
                static std::unordered_map<llvm::GlobalVariable*, void*> made;

                void* &r = made[gv];
                if (r == NULL) {
                    llvm::Type *t = gv->getType()->getElementType();
                    r = (void*)malloc(width(t, dl));
                    if (gv->hasInitializer()) {
                        llvm::Constant* init = gv->getInitializer();
                        assert(init->getType() == t);
                        if (t == g.i64) {
                            llvm::ConstantInt *ci = llvm::cast<llvm::ConstantInt>(init);
                            *(int64_t*)r = ci->getSExtValue();
                        } else {
                            gv->dump();
                            RELEASE_ASSERT(0, "");
                        }
                    }
                }

                //gv->getType()->dump();
                //gv->dump();
                //printf("%p\n", r);
                //RELEASE_ASSERT(0, "");
                return (int64_t)r;
            }

            gv->dump();
            RELEASE_ASSERT(0, "");
        }
        case llvm::Value::UndefValueVal:
            return (int64_t)-1337;
        default:
            v->dump();
            RELEASE_ASSERT(0, "%d", v->getValueID());
    }
}

static void set(SymMap &symbols, const llvm::BasicBlock::iterator &it, Val v) {
    if (VERBOSITY() >= 2) {
        printf("Setting to %lx / %f: ", v.n, v.d);
        fflush(stdout);
        it->dump();
    }

    SymMap::iterator f = symbols.find(it);
    if (f != symbols.end())
        f->second = v;
    else
        symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*it)), v));
//#define SET(v) symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*it)), Val(v)))
}

static std::unordered_map<void*, const SymMap*> interpreter_roots;
void gatherInterpreterRootsForFrame(GCVisitor *visitor, void* frame_ptr) {
    auto it = interpreter_roots.find(frame_ptr);
    if (it == interpreter_roots.end()) {
        printf("%p is not an interpreter frame; they are", frame_ptr);
        for (auto it2 : interpreter_roots) {
            printf(" %p", it2.first);
        }
        printf("\n");
        abort();
    }

    //printf("Gathering roots for frame %p\n", frame_ptr);
    const SymMap* symbols = it->second;

    for (auto it2 : *symbols) {
        visitor->visitPotential(it2.second.o);
    }
}

class UnregisterHelper {
    private:
        void* frame_ptr;

    public:
        constexpr UnregisterHelper(void* frame_ptr) : frame_ptr(frame_ptr) {}

        ~UnregisterHelper() {
            assert(interpreter_roots.count(frame_ptr));
            interpreter_roots.erase(frame_ptr);
        }
};

Box* interpretFunction(llvm::Function *f, int nargs, Box* arg1, Box* arg2, Box* arg3, Box* *args) {
    assert(f);

#ifdef TIME_INTERPRETS
    Timer _t("to interpret", 1000000);
    long this_us = 0;
#endif

    static StatCounter interpreted_runs("interpreted_runs");
    interpreted_runs.log();

    llvm::DataLayout dl(f->getParent());

    //f->dump();
    //assert(nargs == f->getArgumentList().size());

    SymMap symbols;

    void* frame_ptr = __builtin_frame_address(0);
    interpreter_roots[frame_ptr] = &symbols;
    UnregisterHelper helper(frame_ptr);

    int i = 0;
    for (llvm::Function::arg_iterator AI = f->arg_begin(), end = f->arg_end(); AI != end; AI++, i++) {
        if (i == 0) symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*AI)), Val(arg1)));
        else if (i == 1) symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*AI)), Val(arg2)));
        else if (i == 2) symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*AI)), Val(arg3)));
        else {
            assert(i == 3);
            assert(f->getArgumentList().size() == 4);
            assert(f->getArgumentList().back().getType() == g.llvm_value_type_ptr->getPointerTo());
            symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*AI)), Val((int64_t)args)));
            //printf("loading %%4 with %p\n", (void*)args);
            break;
        }
    }

    llvm::BasicBlock *prevblock = NULL;
    llvm::BasicBlock *curblock = &f->getEntryBlock();


    while (true) {
        for (llvm::BasicBlock::iterator it = curblock->begin(), end = curblock->end(); it != end; ++it) {
            if (VERBOSITY("interpreter") >= 2) {
                printf("executing in %s: ", f->getName().data());
                fflush(stdout);
                it->dump();
                //f->dump();
            }

#define SET(v) set(symbols, it, (v))
            if (llvm::LoadInst *li = llvm::dyn_cast<llvm::LoadInst>(it)) {
                llvm::Value *ptr = li->getOperand(0);
                Val v = fetch(ptr, dl, symbols);
                //printf("loading from %p\n", v.o);

                if (width(li, dl) == 1) {
                    Val r = Val(*(bool*)v.o);
                    SET(r);
                    continue;
                } else if (width(li, dl) == 8) {
                    Val r = Val(*(int64_t*)v.o);
                    SET(r);
                    continue;
                } else {
                    li->dump();
                    RELEASE_ASSERT(0, "");
                }
            } else if (llvm::StoreInst *si = llvm::dyn_cast<llvm::StoreInst>(it)) {
                llvm::Value *val = si->getOperand(0);
                llvm::Value *ptr = si->getOperand(1);
                Val v = fetch(val, dl, symbols);
                Val p = fetch(ptr, dl, symbols);

                //printf("storing %lx at %lx\n", v.n, p.n);

                if (width(val, dl) == 1) {
                    *(bool*)p.o = v.b;
                    continue;
                } else if (width(val, dl) == 8) {
                    *(int64_t*)p.o = v.n;
                    continue;
                } else {
                    si->dump();
                    RELEASE_ASSERT(0, "");
                }
            } else if (llvm::CmpInst *ci = llvm::dyn_cast<llvm::CmpInst>(it)) {
                assert(ci->getType() == g.i1);

                Val a0 = fetch(ci->getOperand(0), dl, symbols);
                Val a1 = fetch(ci->getOperand(1), dl, symbols);
                llvm::CmpInst::Predicate pred = ci->getPredicate();
                switch (pred) {
                    case llvm::CmpInst::ICMP_EQ:
                        SET(a0.n == a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_NE:
                        SET(a0.n != a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SLT:
                        SET(a0.n < a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SLE:
                        SET(a0.n <= a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SGT:
                        SET(a0.n > a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SGE:
                        SET(a0.n >= a1.n);
                        continue;
                    case llvm::CmpInst::FCMP_OEQ:
                        SET(a0.d == a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_UNE:
                        SET(a0.d != a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OLT:
                        SET(a0.d < a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OLE:
                        SET(a0.d <= a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OGT:
                        SET(a0.d > a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OGE:
                        SET(a0.d >= a1.d);
                        continue;
                    default:
                        ci->dump();
                        RELEASE_ASSERT(0, "");
                }
                continue;
            } else if (llvm::BinaryOperator *bo = llvm::dyn_cast<llvm::BinaryOperator>(it)) {
                if (bo->getOperand(0)->getType() == g.i64 || bo->getOperand(0)->getType() == g.i1) {
                    //assert(bo->getOperand(0)->getType() == g.i64);
                    //assert(bo->getOperand(1)->getType() == g.i64);

                    Val a0 = fetch(bo->getOperand(0), dl, symbols);
                    Val a1 = fetch(bo->getOperand(1), dl, symbols);
                    llvm::Instruction::BinaryOps opcode = bo->getOpcode();
                    switch (opcode) {
                        case llvm::Instruction::Add:
                            SET(a0.n + a1.n);
                            continue;
                        case llvm::Instruction::And:
                            SET(a0.n & a1.n);
                            continue;
                        case llvm::Instruction::AShr:
                            SET(a0.n >> a1.n);
                            continue;
                        case llvm::Instruction::Mul:
                            SET(a0.n * a1.n);
                            continue;
                        case llvm::Instruction::Or:
                            SET(a0.n | a1.n);
                            continue;
                        case llvm::Instruction::Shl:
                            SET(a0.n << a1.n);
                            continue;
                        case llvm::Instruction::Sub:
                            SET(a0.n - a1.n);
                            continue;
                        case llvm::Instruction::Xor:
                            SET(a0.n ^ a1.n);
                            continue;
                        default:
                            bo->dump();
                            RELEASE_ASSERT(0, "");
                    }
                    continue;
                } else if (bo->getOperand(0)->getType() == g.double_) {
                    //assert(bo->getOperand(0)->getType() == g.i64);
                    //assert(bo->getOperand(1)->getType() == g.i64);

                    double lhs = fetch(bo->getOperand(0), dl, symbols).d;
                    double rhs = fetch(bo->getOperand(1), dl, symbols).d;
                    llvm::Instruction::BinaryOps opcode = bo->getOpcode();
                    switch (opcode) {
                        case llvm::Instruction::FAdd:
                            SET(lhs + rhs);
                            continue;
                        case llvm::Instruction::FMul:
                            SET(lhs * rhs);
                            continue;
                        case llvm::Instruction::FSub:
                            SET(lhs - rhs);
                            continue;
                        default:
                            bo->dump();
                            RELEASE_ASSERT(0, "");
                    }
                    continue;
                } else {
                    bo->dump();
                    RELEASE_ASSERT(0, "");
                }
            } else if (llvm::GetElementPtrInst *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(it)) {
                int64_t base = fetch(gep->getPointerOperand(), dl, symbols).n;

                llvm::User::value_op_iterator begin = gep->value_op_begin();
                ++begin;
                std::vector<llvm::Value*> indices(begin, gep->value_op_end());

                int64_t offset = dl.getIndexedOffset(gep->getPointerOperandType(), indices);
                //gep->dump();
                //printf("offset for inst: %ld (base is %lx)\n", offset, base);
                SET(base + offset);
                continue;
            } else if (llvm::AllocaInst *al = llvm::dyn_cast<llvm::AllocaInst>(it)) {
                int size = fetch(al->getArraySize(), dl, symbols).n * width(al->getAllocatedType(), dl);
                void* ptr = alloca(size);
                //void* ptr = malloc(size);
                //printf("alloca()'d at %p\n", ptr);
                SET((int64_t)ptr);
                continue;
            } else if (llvm::SIToFPInst *si = llvm::dyn_cast<llvm::SIToFPInst>(it)) {
                assert(width(si->getOperand(0), dl) == 8);
                SET((double)fetch(si->getOperand(0), dl, symbols).n);
                continue;
            } else if (llvm::BitCastInst *bc = llvm::dyn_cast<llvm::BitCastInst>(it)) {
                assert(width(bc->getOperand(0), dl) == 8);
                SET(fetch(bc->getOperand(0), dl, symbols));
                continue;
            } else if (llvm::IntToPtrInst *bc = llvm::dyn_cast<llvm::IntToPtrInst>(it)) {
                assert(width(bc->getOperand(0), dl) == 8);
                SET(fetch(bc->getOperand(0), dl, symbols));
                continue;
            } else if (llvm::CallInst *ci = llvm::dyn_cast<llvm::CallInst>(it)) {
                void* f;
                int arg_start;
                if (ci->getCalledFunction() && (ci->getCalledFunction()->getName() == "llvm.experimental.patchpoint.void" || ci->getCalledFunction()->getName() == "llvm.experimental.patchpoint.i64")) {
                    //ci->dump();
                    f = (void*)fetch(ci->getArgOperand(2), dl, symbols).n;
                    arg_start = 4;
                } else {
                    f = (void*)fetch(ci->getCalledValue(), dl, symbols).n;
                    arg_start = 0;
                }

                if (VERBOSITY("interpreter") >= 2) printf("calling %s\n", g.func_addr_registry.getFuncNameAtAddress(f, true).c_str());

                std::vector<Val> args;
                int nargs = ci->getNumArgOperands();
                for (int i = arg_start; i < nargs; i++) {
                    //ci->getArgOperand(i)->dump();
                    args.push_back(fetch(ci->getArgOperand(i), dl, symbols));
                }

                int npassed_args = nargs - arg_start;
                //printf("%d %d %d\n", nargs, arg_start, npassed_args);

#ifdef TIME_INTERPRETS
                this_us += _t.end();
#endif
                // This is dumb but I don't know how else to do it:

                int mask = 1;
                if (ci->getType() == g.double_)
                    mask = 3;
                else
                    mask = 2;

                for (int i = 0; i < npassed_args; i++) {
                    mask <<= 1;
                    if (ci->getOperand(i)->getType() == g.double_)
                        mask |= 1;
                }

                Val r((int64_t)0);
                switch (mask) {
                    case 0b10:
                        r = reinterpret_cast<int64_t (*)()>(f)();
                        break;
                    case 0b11:
                        r = reinterpret_cast<double (*)()>(f)();
                        break;
                    case 0b100:
                        r = reinterpret_cast<int64_t (*)(int64_t)>(f)(args[0].n);
                        break;
                    case 0b101:
                        r = reinterpret_cast<int64_t (*)(double)>(f)(args[0].d);
                        break;
                    case 0b110:
                        r = reinterpret_cast<double (*)(int64_t)>(f)(args[0].n);
                        break;
                    case 0b1000:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t)>(f)(args[0].n, args[1].n);
                        break;
                    case 0b1001:
                        r = reinterpret_cast<int64_t (*)(int64_t, double)>(f)(args[0].n, args[1].d);
                        break;
                    case 0b1011:
                        r = reinterpret_cast<int64_t (*)(double, double)>(f)(args[0].d, args[1].d);
                        break;
                    case 0b1111:
                        r = reinterpret_cast<double (*)(double, double)>(f)(args[0].d, args[1].d);
                        break;
                    case 0b10000:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t)>(f)(args[0].n, args[1].n, args[2].n);
                        break;
                    case 0b10001:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, double)>(f)(args[0].n, args[1].n, args[2].d);
                        break;
                    case 0b10011:
                        r = reinterpret_cast<int64_t (*)(int64_t, double, double)>(f)(args[0].n, args[1].d, args[2].d);
                        break;
                    case 0b100000:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t)>(f)(args[0].n, args[1].n, args[2].n, args[3].n);
                        break;
                    case 0b100001:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, double)>(f)(args[0].n, args[1].n, args[2].n, args[3].d);
                        break;
                    case 0b100110:
                        r = reinterpret_cast<int64_t (*)(int64_t, double, double, int64_t)>(f)(args[0].n, args[1].d, args[2].d, args[3].n);
                        break;
                    case 0b101010:
                        r = reinterpret_cast<int64_t (*)(double, int, double, int64_t)>(f)(args[0].d, args[1].n, args[2].d, args[3].n);
                        break;
                    case 0b1000000:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t)>(f)(args[0].n, args[1].n, args[2].n, args[3].n, args[4].n);
                        break;
                    case 0b10000000:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)>(f)(args[0].n, args[1].n, args[2].n, args[3].n, args[4].n, args[5].n);
                        break;
                    case 0b100000000:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)>(f)(args[0].n, args[1].n, args[2].n, args[3].n, args[4].n, args[5].n, args[6].n);
                        break;
                    case 0b1000000000:
                        r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)>(f)(args[0].n, args[1].n, args[2].n, args[3].n, args[4].n, args[5].n, args[6].n, args[7].n);
                        break;
                    default:
                        it->dump();
                        RELEASE_ASSERT(0, "%d", mask);
                        break;
                }
                if (ci->getType() != g.void_)
                    SET(r);


#ifdef TIME_INTERPRETS
                _t.restart("to interpret", 10000000);
#endif
                continue;
            } else if (llvm::SelectInst *si = llvm::dyn_cast<llvm::SelectInst>(it)) {
                Val test = fetch(si->getCondition(), dl, symbols);
                Val vt = fetch(si->getTrueValue(), dl, symbols);
                Val vf = fetch(si->getFalseValue(), dl, symbols);
                if (test.b)
                    SET(vt);
                else
                    SET(vf);
                continue;
            } else if (llvm::PHINode *phi = llvm::dyn_cast<llvm::PHINode>(it)) {
                assert(prevblock);
                SET(fetch(phi->getIncomingValueForBlock(prevblock), dl, symbols));
                continue;
            } else if (llvm::BranchInst *br = llvm::dyn_cast<llvm::BranchInst>(it)) {
                prevblock = curblock;
                if (br->isConditional()) {
                    Val t = fetch(br->getCondition(), dl, symbols);
                    if (t.b) {
                        curblock = br->getSuccessor(0);
                    } else {
                        curblock = br->getSuccessor(1);
                    }
                } else {
                    curblock = br->getSuccessor(0);
                }
                //if (VERBOSITY()) {
                    //printf("jumped to %s\n", curblock->getName().data());
                //}
                break;
            } else if (llvm::ReturnInst *ret = llvm::dyn_cast<llvm::ReturnInst>(it)) {
                llvm::Value* r = ret->getReturnValue();

#ifdef TIME_INTERPRETS
                this_us += _t.end();
                static StatCounter us_interpreting("us_interpreting");
                us_interpreting.log(this_us);
#endif

                if (!r)
                    return NULL;
                Val t = fetch(r, dl, symbols);
                return t.o;
            }


            it->dump();
            RELEASE_ASSERT(0, "");
        }
    }

    RELEASE_ASSERT(0, "");
}

}