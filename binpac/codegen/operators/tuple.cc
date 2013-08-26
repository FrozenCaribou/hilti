
#include "cg-operator-common.h"
#include <binpac/autogen/operators/tuple.h>

using namespace binpac;
using namespace binpac::codegen;

void CodeBuilder::visit(constant::Tuple* t)
{
    hilti::builder::tuple::element_list elems;

    for ( auto e : t->value() )
        elems.push_back(cg()->hiltiExpression(e));

    auto result = hilti::builder::tuple::create(elems, t->location());
    setResult(result);
}

void CodeBuilder::visit(expression::operator_::tuple::CoerceTuple* t)
{
    auto op1 = cg()->hiltiExpression(t->op1());

    // For now we cheat here and rely on HILTI to do the coercion right.
    // Otherwise, we'd have to split the tuple apart, coerce each element
    // individually, and then put it back together; something which is
    // probably always unnecessary. However, I'm not sure we'll eventually
    // get around that ...

    setResult(op1);
}

void CodeBuilder::visit(expression::operator_::tuple::Equal* i)
{
    auto result = builder()->addTmp("equal", hilti::builder::boolean::type());
    auto op1 = cg()->hiltiExpression(i->op1());
    auto op2 = cg()->hiltiExpression(i->op2());
    cg()->builder()->addInstruction(result, hilti::instruction::tuple::Equal, op1, op2);
    setResult(result);
}

void CodeBuilder::visit(expression::operator_::tuple::Index* i)
{
    auto tuple = ast::checkedCast<type::Tuple>(i->op1()->type());
    auto types = tuple->typeList();

    auto idx = ast::checkedCast<expression::Constant>(i->op2());
    auto const_ = ast::checkedCast<constant::Integer>(idx->constant());

    auto t = types.begin();
    std::advance(t, const_->value());

    auto result = builder()->addTmp("elem", cg()->hiltiType(*t));
    auto op1 = cg()->hiltiExpression(i->op1());
    auto op2 = cg()->hiltiExpression(i->op2());
    cg()->builder()->addInstruction(result, hilti::instruction::tuple::Index, op1, op2);
    setResult(result);
}