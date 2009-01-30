# $Id$
from hilti.core import *

class Checker(visitor.Visitor):
    def __init__(self):
        super(Checker, self).__init__(all=True)
        self.reset()

    def reset(self):
        self._in_function = False
        self._have_module = False
        self._have_others = False
        self._errors = 0

    def checkAST(self, ast):
        self.reset()
        self.dispatch(ast)
        return self._errors
        
    def error(self, obj, str):
        self._errors += 1
        util.error(str, context=obj.location(), fatal=False)        
        self.skipOthers()

checker = Checker()

        
