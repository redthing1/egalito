#include <capstone/capstone.h>
#include "jumptable.h"
#include "controlflow.h"
#include "chunk/instruction.h"
#include "slicing.h"
#include "slicingtree.h"
#include "slicingmatch.h"

#include "types.h"
#include "log/log.h"

long JumpTableDescriptor::getEntries() const {
    if(!isBoundKnown()) return -1;
    return bound + 1;
}

void JumpTableSearch::search(Module *module) {
    for(auto f : module->getChildren()->getIterable()->iterable()) {
        search(f);
    }
}

void JumpTableSearch::search(Function *function) {
    ControlFlowGraph cfg(function);

    for(auto b : function->getChildren()->getIterable()->iterable()) {
        auto i = b->getChildren()->getIterable()->getLast();
        if(dynamic_cast<IndirectJumpInstruction *>(i->getSemantic())) {
            SlicingSearch search(&cfg);
            search.sliceAt(i);

            JumpTableDescriptor descriptor(function, i);

            if(matchJumpTable(search.getInitialState(), &descriptor)
                && (matchJumpTableBounds(&search, &descriptor)
                    || savePartialInfoTables)) {

                LOG(1, "FOUND JUMP TABLE BY PATTERN MATCHING!!!");
                tableList.push_back(new JumpTableDescriptor(descriptor));
            }
        }
    }
}

bool JumpTableSearch::matchJumpTable(SearchState *state,
    JumpTableDescriptor *d) {

    auto i = state->getInstruction();
    auto v = dynamic_cast<IndirectJumpInstruction *>(i->getSemantic());
    if(!v) return false;

    // get final tree for pattern matching
    auto tree = state->getRegTree(v->getRegister());

#ifdef ARCH_X86_64
    typedef TreePatternRegisterIs<X86_REG_RIP> TreePatternRIP;

    typedef TreePatternBinary<TreeNodeAddition,
        TreePatternTerminal<TreeNodeAddress>,
        TreePatternRIP
    > TreePatternLEA;

    typedef TreePatternBinary<TreeNodeAddition,
        TreePatternLEA,
        TreePatternBinary<TreeNodeAddition,
            TreePatternCapture<TreePatternLEA>,
            TreePatternBinary<TreeNodeMultiplication,
                TreePatternCapture<TreePatternAny>,
                TreePatternConstantIs<4>>
        >
    > Form1;
#elif defined(ARCH_AARCH64)
    typedef TreePatternTerminal<TreeNodeAddress> TreePatternTargetBase;

    // base address could have been saved on stack
    typedef TreePatternBinary<TreeNodeAddition,
        TreePatternCapture<TreePatternAny>,
        TreePatternCapture<TreePatternAny>
    > TreePatternTableEntry;

    typedef TreePatternBinary<TreeNodeLogicalShiftLeft,
        TreePatternUnary<TreeNodeDereference, TreePatternTableEntry>,
        TreePatternConstantIs<2>
    > TreePatternTargetOffset;

    typedef TreePatternBinary<TreeNodeAddition,
            TreePatternTargetBase,
            TreePatternTargetOffset
    > Form1;
#endif

    TreeCapture capture;
    if(Form1::matches(tree, capture)) {
        LOG(1, "found jump table jump:");

#ifdef ARCH_X86_64
        LOG0(1, "    address of jump table: ");
        auto node = dynamic_cast<TreeNodeAddition *>(capture.get(0));
        auto left = dynamic_cast<TreeNodeAddress *>(node->getLeft());
        auto right = dynamic_cast<TreeNodeRegisterRIP *>(node->getRight());
        IF_LOG(1) capture.get(0)->print(TreePrinter(1, 0));
        LOG(1, "  => 0x" << std::hex << left->getValue() + right->getValue());

        LOG0(1, "    indexing expression:   ");
        IF_LOG(1) capture.get(1)->print(TreePrinter(1, 0));
        LOG(1, "");

        d->setAddress(left->getValue() + right->getValue());
        d->setScale(4);
        d->setIndexExpr(capture.get(1));
        // indexRegister is not known right now.
#elif defined(ARCH_AARCH64)
        TreeNode *tableAddress = capture.get(0);
        LOG0(1, "    address of jump table: ");
        IF_LOG(1) tableAddress->print(TreePrinter(1, 0));
        std::vector<address_t> baseAddresses = getTableAddresses(state,
                                                                 tableAddress);
        if(baseAddresses.size() > 1) {
            LOG(1, "-- considering only the first table");
        }
        LOG(1, "  => 0x" << std::hex << baseAddresses.front());

        TreeNode *indexExpr = capture.get(1);
        if (auto p = dynamic_cast<TreeNodeLogicalShiftLeft *>(indexExpr)) {
            indexExpr = p->getLeft();
        }
        LOG0(1, "    indexing expression:   ");
        IF_LOG(1) indexExpr->print(TreePrinter(1, 0));
        LOG(1, "");


        d->setAddress(baseAddresses.front());
        d->setScale(4);
        d->setIndexExpr(indexExpr);
#endif
        return true;
    }

    return false;
}

bool JumpTableSearch::matchJumpTableBounds(SlicingSearch *search,
    JumpTableDescriptor *d) {

    for(auto state : search->getConditionList()) {
#ifdef ARCH_X86_64
        auto tree = state->getRegTree(X86_REG_EFLAGS);
#elif defined(ARCH_AARCH64)
        auto tree = state->getRegTree(ARM64_REG_NZCV);
#endif
        auto condition = dynamic_cast<TreeNodeComparison *>(tree);
        if(!condition) continue;

        auto leftGeneric = condition->getLeft();
        auto rightGeneric = condition->getRight();
        auto left = dynamic_cast<TreeNodeConstant *>(condition->getLeft());
        auto right = dynamic_cast<TreeNodeConstant *>(condition->getRight());
        if(!left && !right) continue;

        enum Operator {
            OP_LT = 1,
            OP_LE = 2,
            OP_NE = 4,
            OP_EQ = 10-OP_NE,
            OP_GE = 10-OP_LT,
            OP_GT = 10-OP_LE
        } op;
        const char *opString[] = {0, "<", "<=", 0, "!=", 0, "==", 0, ">=", ">"};

        auto semantic = state->getInstruction()->getSemantic();
        auto v = dynamic_cast<ControlFlowInstruction *>(semantic);
        if(!v) continue;
        std::string mnemonic = v->getMnemonic();
#ifdef ARCH_X86_64
        if(mnemonic == "ja") op = OP_GT;
        else if(mnemonic == "jae") op = OP_GE;
        else if(mnemonic == "jb") op = OP_LT;
        else if(mnemonic == "jbe") op = OP_LE;
        else if(mnemonic == "jne") op = OP_NE;
        else if(mnemonic == "je") op = OP_EQ;
        else if(mnemonic == "jg") op = OP_GT;
        else if(mnemonic == "jge") op = OP_GE;
        else if(mnemonic == "jl") op = OP_LT;
        else if(mnemonic == "jle") op = OP_LE;
        else if(mnemonic == "js") {
            return false;  // this doesn't seem useful...
        }
#elif defined(ARCH_AARCH64)
        if(mnemonic == "b.ls") op = OP_LT;
        else if(mnemonic == "b.eq") op = OP_EQ;
#endif
        else {
            LOG(1, "what is " << mnemonic << "?");
            std::cerr << "what is " << mnemonic << "?\n";
            throw "unimplemented mnemonic in jump table slicing";
        }

        auto taken = state->getJumpTaken();
        if(left && !right) {
            auto t = left;
            left = right;
            right = t;
            auto tt = leftGeneric;
            leftGeneric = rightGeneric;
            rightGeneric = tt;

            op = Operator(10-int(op));
            taken = !taken;
        }

        if(!taken) {
            op = Operator(10-int(op));
        }

        unsigned long bound = right->getValue();
        LOG0(1, "comparison of ");
        IF_LOG(1) leftGeneric->print(TreePrinter(2, 0));
        LOG(1, " is " << opString[op] << " " << std::dec << bound);

        auto indexExpr = d->getIndexExpr();
        if(leftGeneric == indexExpr
            && (op == OP_LE || op == OP_LT)) {

            LOG0(1, "BOUNDS CHECK FOUND! ");
            IF_LOG(1) d->getIndexExpr()->print(TreePrinter(2, 0));
            LOG(1, " is " << opString[op] << " " << std::dec << bound);

            if(op == OP_LT) bound --;  // convert "<" to "<="
            d->setBound(bound);

            return true;
        }
    }

    return false;
}

std::vector<address_t> JumpTableSearch::getTableAddresses(SearchState *state,
    TreeNode *tree) {

    typedef TreePatternUnary<TreeNodeDereference,
        TreePatternBinary<TreeNodeAddition,
            TreePatternCapture<TreePatternTerminal<TreeNodeConstant>>,
            TreePatternCapture<TreePatternAny>
        >
    > TreePatternTableBase;
    std::vector<address_t> baseAddresses;

    if(auto a = dynamic_cast<TreeNodeAddress *>(tree)) {
        baseAddresses.push_back(a->getValue());
        return baseAddresses;
    }

    if(dynamic_cast<TreeNodeMultipleParents *>(tree)) {
        throw "multiple tables used for one table jump?";
        // needs to be handled recursively
    }

    TreeCapture cap1;
    if(!TreePatternTableBase::matches(tree, cap1)) {
        LOG(1, "doesn't match the table base pattern");
    }
    else {
        TreeCapture cap2;
        for(auto const &m : state->getMemTree()) {
            if(auto base = dynamic_cast<TreeNodeAddress *>(m.second)) {
                LOG0(1, "search for ");
                IF_LOG(1) m.first->print(TreePrinter(2, 0));
                LOG(1, "");
                if(TreePatternTableBase::matches(m.first, cap2)) {
                    LOG(1, "(matches table base pattern)");
                    auto c1 = dynamic_cast<TreeNodeConstant *>(cap1.get(0));
                    auto c2 = dynamic_cast<TreeNodeConstant *>(cap2.get(0));
                    if(c1->getValue() == c2->getValue()) {
                        address_t ba = c1->getValue() + base->getValue();
                        LOG(1, "the best candidate: "
                            << c1->getValue()
                            << " + "
                            << base->getValue()
                            << " = " << ba);
                        baseAddresses.push_back(ba);
                    }
                }
            }
        }
    }

    return baseAddresses;
}

