#! $Id$ 
#
# BinPAC++ statements

import ast
import scope
import type
import id
import binpac.support.util as util

import hilti.core.instruction

class Statement(ast.Node):
    """Base class for BinPAC++ statements.
    
    location: ~~Location - The location where the expression was defined. 
    """
    def __init__(self, location=None):
        super(Statement, self).__init__(location=location)
    
    # Methods for derived classes to override.
    
    def execute(self, cg):
        """Generates HILTI code executing the statement.
        
        Must be overidden by derived classes.
        
        cg: ~~Codegen - The current code generator.
        """
        util.internal_error("Statement.execute() not overidden by %s" % self.__class__)

    def __str__(self):
        return "<generic statement>"
        
class Block(Statement):
    """A block of statements executed in sequence. Such a block has its own
    local scope. 
    
    pscope: ~~Scope - The parent scope for this block.
    location: ~~Location - The location where the expression was defined. 
    """
    def __init__(self, pscope, location=None):
        super(Block, self).__init__(location=location)
        self._stmts = []
        self._scope = scope.Scope(None, pscope)

    def scope(self):
        """Returns the block's scope.
        
        Returns: ~~Scope - The scope.
        """
        return self._scope

    def statements(self):
        """Returns the block's statements.
        
        Returns: list of ~~Statement - The statements.
        """
        return self._stmts
    
    def addStatement(self, stmt):
        """Adds a statement to the block.
        stmt: ~~Statement - The statement to add.
        """
        self._stmts += [stmt]
    
    ### Overidden from ast.Node.

    def validate(self, vld):
        for stmt in self._stmts:
            stmt.validate(vld)
            
    def pac(self, printer):
        printer.push()
        printer.output("{", nl=True)
        for stmt in self._stmts:
            stmt.pac(printer)
        printer.output("}", nl=True)
        printer.pop()
    
    ### Overidden from Statement.

    def execute(self, cg):
        for i in self._scope.IDs():
            if isinstance(i, id.Local):
                cg.builder().addLocal(i.name(), i.type().hiltiType(cg))
        
        for stmt in self._stmts:
            stmt.execute(cg)

    def __str__(self):
        return "<statement block>"

class FieldHook(Block):
    """A field hook is a series of statements executed when a unit field has
    been parsed.
    
    field: ~~Field - The field to which hook is attached. 
    
    prio: int - The priority of the statement. If multiple statements are
    defined for the same field, they are executed in order of decreasing
    priority.
    
    stms: list of ~~Statement - Series of statements to initalize the hook with.
    """
    def __init__(self, field, prio, stmts=[], location=None):
        self._field = field
        self._prio = prio
        
        super(FieldHook, self).__init__(field.parent().scope(), location=location)

        for stmt in stmts:
            self.addStatement(stmt)

    def field(self):
        """Returns the field associated with the hook.
        
        Returns: ~~Field - The field.
        """
        return self._field
            
    def priority(self):
        """Returns the priority associated with the hook.  If multiple statements are
        defined for the same field, they are executed in order of decreasing
        priority.
        
        Returns: int - The priority.
        """
        return self._prio
        
    def params(self):
        """Returns parameters to passed into the hook.
        
        Returns: list of ~~id.Parameter - The paramemters.
        """
        ddt = self._field.type().dollarDollarType(self._field)

        if not ddt:
            return []
        
        return [id.Parameter("__dollardollar", ddt, location=self.location())] if ddt else []

    # Overriden from Block.
    
    def scope(self):
        # If our parameters aren't yet in there, add them. Can't do that in
        # the ctor because types might not be resolved at that time.
        scope = super(FieldHook, self).scope()
        
        for p in self.params():
            if not scope.lookupID(p.name()):
                scope.addID(p)
                
        return scope
    
class Print(Statement):
    """The ``print`` statement.
    
    exprs: list of ~Expression - The arguments to print.
    
    location: ~~Location - The location where the expression was defined. 
    """
    def __init__(self, exprs, location=None):
        super(Print, self).__init__(location=location)
        self._exprs = exprs
        
    ### Overidden from ast.Node.
    
    def validate(self, vld):
        for expr in self._exprs:
            expr.validate(vld)
            
            if isinstance(expr.type(), type.Void):
                vld.error(expr, "cannot print void expressions")

    def pac(self, printer):
        printer.output("print ")
        for i in range(len(self._exprs)):
            self._exprs[i].pac(printer)
            if i != len(self._exprs) - 1:
                printer.output(", ")
                
        printer.output(";", nl=True)
    
    ### Overidden from Statement.
        
    def execute(self, cg):
        func = cg.builder().idOp("Hilti::print")
        
        if len(self._exprs):
            ops = [expr.evaluate(cg) for expr in self._exprs]
        else:
            # Print an empty line.
            ops = [cg.builder().constOp("")]
        
        for i in range(len(ops)):
            builder = cg.builder()
            nl = builder.constOp(i == len(ops) - 1)
            builder.call(None, func, builder.tupleOp([ops[i], nl]))
        
    def __str__(self):
        return "<print statement>"
    
class Expression(Statement):
    """A statement evaluating an expression.

    expr: ~~Expression - The expression to evaluate.
    
    location: ~~Location - The location where the statement was defined. 
    """
    def __init__(self, expr, location=None):
        super(Expression, self).__init__(location=location)
        self._expr = expr
        
    ### Overidden from ast.Node.
    
    def validate(self, vld):
        self._expr.validate(vld)

    def pac(self, printer):
        self._expr.pac(printer)
        printer.output(";", nl=True)
    
    ### Overidden from Statement.
        
    def execute(self, cg):
        self._expr.evaluate(cg)
        
    def __str__(self):
        return str(self._expr) + ";"
    
