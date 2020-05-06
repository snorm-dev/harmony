import sys
import random

def islower(c):
    return c in "abcdefghijklmnopqrstuvwxyz"

def isupper(c):
    return c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

def isletter(c):
    return islower(c) or isupper(c)

def isnumeral(c):
    return c in "0123456789"

def isalnum(c):
    return isletter(c) or isnumeral(c)

def isnamechar(c):
    return isalnum(c) or c == "_"

def isprint(c):
    return isinstance(c, str) and len(c) == 1 and (
        isalnum(c) or c in " ~`!@#$%^&*()-_=+[{]}\\|;:'\",<.>/?")

def isnumber(s):
    return all(isnumeral(c) for c in s)

def isname(s):
    return (isletter(s[0]) or s[0] == "_") and all(isnamechar(c) for c in s)

tokens = [ "{<", ">}", ":=", "==", "!=", "<=", ">=", "..", "/\\", "\\/",
            "&(", "!(", "choose(" ]

def lexer(s, file):
    result = []
    line = 1
    column = 1
    while s != "":
        # see if it's a blank
        if s[0] in { " ", "\t" }:
            s = s[1:]
            column += 1
            continue

        if s[0] == "\n":
            s = s[1:]
            line += 1
            column = 1
            continue

        # skip over line comments
        if s.startswith("//"):
            s = s[2:]
            while len(s) > 0 and s[0] != '\n':
                s = s[1:]
            continue

        # skip over nested comments
        if s.startswith("(*"):
            count = 1
            s = s[2:]
            column += 2
            while count != 0 and s != "":
                if s.startswith("(*"):
                    count += 1
                    s = s[2:]
                    column += 2
                elif s.startswith("*)"):
                    count -= 1
                    s = s[2:]
                    column += 2
                elif s[0] == "\n":
                    s = s[1:]
                    line += 1
                    column = 1
                else:
                    s = s[1:]
                    column += 1
            continue

        # see if it's a multi-character token.  Match with the longest one
        found = ""
        for t in tokens:
            if s.startswith(t) and len(t) > len(found):
                found = t
        if found != "":
            result += [ (found, file, line, column) ]
            s = s[len(found):]
            column += len(found)
            continue

        # see if a sequence of letters and numbers
        if isnamechar(s[0]):
            i = 0
            while i < len(s) and isnamechar(s[i]):
                i += 1
            result += [ (s[:i], file, line, column) ]
            s = s[i:]
            column += i
            continue

        # string
        if s[0] == '"':
            i = 1
            str = '"'
            while i < len(s) and s[i] != '"':
                if s[i] == '\\':
                    i += 1
                    if i == len(s):
                        break
                    if s[i] == '"':
                        str += '"'
                    elif s[i] == '\\':
                        str += '\\'
                    elif s[i] == 't':
                        str += '\t'
                    elif s[i] == 'n':
                        str += '\n'
                    elif s[i] == 'f':
                        str += '\f'
                    elif s[i] == 'r':
                        str += '\r'
                    else:
                        str += s[i]
                else:
                    str += s[i]
                i += 1
            if i < len(s):
                i += 1
            str += '"'
            result += [ (str, file, line, column) ]
            s = s[i:]
            column += i
            continue

        # everything else is a single character token
        result += [ (s[0], file, line, column) ]
        s = s[1:]
        column += 1
    return result

class Value:
    pass

class NoValue(Value):
    def __repr__(self):
        return "NoValue()"

    def __hash__(self):
        return 0

    def __eq__(self, other):
        return isinstance(other, NoValue)

class PcValue(Value):
    def __init__(self, pc):
        self.pc = pc

    def __repr__(self):
        return "PC(" + str(self.pc) + ")"

    def __hash__(self):
        return self.pc

    def __eq__(self, other):
        return isinstance(other, PcValue) and other.pc == self.pc

class RecordValue(Value):
    def __init__(self, d):
        self.d = d

    def __repr__(self):
        return str(self.d)

    def __hash__(self):
        hash = 0
        for x in self.d.items():
            hash ^= x.__hash__()
        return hash

    # Two dictionaries are the same if they have the same (key, value) pairs
    def __eq__(self, other):
        if not isinstance(other, RecordValue):
            return False
        if len(self.d.keys()) != len(other.d.keys()):
            return False
        for (k, v) in self.d.items():
            if v != other.d.get(k):
                return False
        return True

    def __len__(self):
        return len(self.d.keys())

class NameValue(Value):
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return self.name

    def __hash__(self):
        return self.name.__hash__()

    def __eq__(self, other):
        if not isinstance(other, NameValue):
            return False
        return self.name == other.name

class SetValue(Value):
    def __init__(self, s):
        self.s = s

    def __repr__(self):
        return str(self.s)

    def __hash__(self):
        return frozenset(self.s).__hash__()

    def __eq__(self, other):
        if not isinstance(other, SetValue):
            return False
        return self.s == other.s

class AddressValue(Value):
    def __init__(self, indexes):
        self.indexes = indexes

    def __repr__(self):
        return "AV(" + str(self.indexes) + ")"

    def __hash__(self):
        hash = 0
        for x in self.indexes:
            hash ^= x.__hash__()
        return hash

    def __eq__(self, other):
        if not isinstance(other, AddressValue):
            return False
        return self.indexes == other.indexes

class Op:
    pass

class LoadOp(Op):
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return "Load " + str(self.name)

    def eval(self, state, context):
        (lexeme, file, line, column) = self.name
        context.stack.append(state.get(lexeme))
        context.pc += 1

class LoadVarOp(Op):
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return "LoadVar " + str(self.name)

    def eval(self, state, context):
        (lexeme, file, line, column) = self.name
        context.stack.append(context.get(lexeme))
        context.pc += 1

class ConstantOp(Op):
    def __init__(self, constant):
        self.constant = constant

    def __repr__(self):
        return "Constant " + str(self.constant)

    def eval(self, state, context):
        (lexeme, file, line, column) = self.constant
        context.stack.append(lexeme)
        context.pc += 1

class LabelOp(Op):
    def __init__(self, label):
        self.label = label

    def __repr__(self):
        return "Label " + str(self.label)

    def eval(self, state, context):
        context.pc += 1

class StoreOp(Op):
    def __init__(self, n):
        self.n = n                  # #indexes

    def __repr__(self):
        return "Store " + str(self.n)

    def eval(self, state, context):
        indexes = []
        for i in range(self.n):
            indexes.append(context.stack.pop())
        state.set(indexes, context.stack.pop())
        context.pc += 1

class StoreIndOp(Op):
    def __init__(self, n):
        self.n = n                  # #indexes

    def __repr__(self):
        return "StoreInd " + str(self.n)

    def eval(self, state, context):
        indexes = []
        for i in range(self.n):
            indexes.append(context.stack.pop())
        av = indexes[0]
        assert isinstance(av, AddressValue)
        state.set(av.indexes + indexes[1:], context.stack.pop())
        context.pc += 1

class AddressOp(Op):
    def __init__(self, n):
        self.n = n          # #indexes in LValue

    def __repr__(self):
        return "Address " + str(self.n)

    def eval(self, state, context):
        indexes = []
        for i in range(self.n):
            indexes.append(context.stack.pop())
        context.stack.append(AddressValue(indexes))
        context.pc += 1

class AddressIndOp(Op):
    def __init__(self, n):
        self.n = n          # #indexes in LValue

    def __repr__(self):
        return "Address " + str(self.n)

    def eval(self, state, context):
        indexes = []
        for i in range(self.n):
            indexes.append(context.stack.pop())
        av = indexes[0]
        assert isinstance(av, AddressValue), av
        context.stack.append(AddressValue(av.indexes + indexes[1:]))
        context.pc += 1

class LockOp(Op):
    def __init__(self, n):
        self.n = n

    def __repr__(self):
        return "Lock " + str(self.n)

    def eval(self, state, context):
        indexes = []
        for i in range(self.n):
            indexes.append(context.stack.pop())
        state.set(indexes, True)
        context.pc += 1

class StoreVarOp(Op):
    def __init__(self, v, n):
        self.v = v
        self.n = n

    def __repr__(self):
        return "StoreVar " + str(self.v) + " " + str(self.n)

    def eval(self, state, context):
        (lexeme, file, line, column) = self.v
        indexes = []
        for i in range(self.n):
            indexes.append(context.stack.pop())
        context.set([lexeme] + indexes, context.stack.pop())
        context.pc += 1

class PointerOp(Op):
    def __repr__(self):
        return "Pointer"

    def eval(self, state, context):
        av = context.stack.pop()
        assert isinstance(av, AddressValue), av
        context.stack.append(state.iget(av.indexes))
        context.pc += 1

class TasOp(Op):
    def __repr__(self):
        return "TAS"

    def eval(self, state, context):
        av = context.stack.pop()
        assert isinstance(av, AddressValue), av
        context.stack.append(state.iget(av.indexes))
        state.set(av.indexes, True)
        context.pc += 1

class ChooseOp(Op):
    def __repr__(self):
        return "Choose"

class AssertOp(Op):
    def __repr__(self):
        return "Assert"

    def eval(self, state, context):
        expr = context.stack.pop()
        cond = context.stack.pop()
        assert isinstance(cond, bool)
        assert cond, expr           # TODO.  Should print trace instead
        context.pc += 1

class PopOp(Op):
    def __init__(self):
        pass

    def __repr__(self):
        return "Pop"

    def eval(self, state, context):
        context.stack.pop()
        context.pc += 1

class RoutineOp(Op):
    def __init__(self, name, endpc):
        self.name = name
        self.endpc = endpc      # points to return code

    def __repr__(self):
        return "Routine " + str(self.name) + " " + str(self.endpc)

    def eval(self, state, context):
        context.pc = self.endpc + 1

class FrameOp(Op):
    def __repr__(self):
        return "Frame"

    def eval(self, state, context):
        arg = context.stack.pop()
        context.stack.append(context.vars)
        context.vars = RecordValue({ "self": arg })
        context.pc += 1

class ReturnOp(Op):
    def __repr__(self):
        return "Return"

    def eval(self, state, context):
        result = context.get("self")
        context.vars = context.stack.pop()
        context.pc = context.stack.pop()
        context.stack.append(result)

class SpawnOp(Op):
    def __repr__(self):
        return "Spawn"

    def eval(self, state, context):
        func = context.stack.pop()
        assert isinstance(func, PcValue)
        ro = state.code[func.pc]
        assert isinstance(ro, RoutineOp), func
        arg = context.stack.pop()
        ctx = Context(ro.name[0], arg, func.pc + 1, ro.endpc)
        ctx.stack.append(arg)
        state.contexts[(ctx.name, ctx.id)] = ctx
        context.pc += 1

class JumpOp(Op):
    def __init__(self, pc):
        self.pc = pc

    def __repr__(self):
        return "Jump " + str(self.pc)

    def eval(self, state, context):
        context.pc = self.pc

class JumpFalseOp(Op):
    def __init__(self, pc):
        self.pc = pc

    def __repr__(self):
        return "JumpFalse " + str(self.pc)

    def eval(self, state, context):
        c = context.stack.pop()
        assert isinstance(c, bool), c
        if c:
            context.pc += 1
        else:
            context.pc = self.pc

class SetOp(Op):
    def __init__(self, nitems):
        self.nitems = nitems

    def __repr__(self):
        return "Set " + str(self.nitems)

    def eval(self, state, context):
        s = set()
        for i in range(self.nitems):
            s.add(context.stack.pop())
        context.stack.append(SetValue(s))
        context.pc += 1

class RecordOp(Op):
    def __init__(self, nitems):
        self.nitems = nitems

    def __repr__(self):
        return "Record " + str(self.nitems)

    def eval(self, state, context):
        d = {}
        for i in range(self.nitems):
            k = context.stack.pop()
            v = context.stack.pop()
            d[k] = v
        context.stack.append(RecordValue(d))
        context.pc += 1

class NaryOp(Op):
    def __init__(self, op, n):
        self.op = op
        self.n = n

    def __repr__(self):
        return "NaryOp " + str(self.op) + " " + str(self.n)

    def eval(self, state, context):
        (op, file, line, column) = self.op
        if self.n == 1:
            e = context.stack.pop()
            if op == "-":
                assert isinstance(e, int)
                context.stack.append(-e)
            elif op == "not":
                assert isinstance(e, bool)
                context.stack.append(not e)
            else:
                assert False, self
        elif self.n == 2:
            e1 = context.stack.pop()
            e2 = context.stack.pop()
            if op == "==":
                context.stack.append(e1 == e2)
            elif op == "+":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 + e2)
            elif op == "-":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 - e2)
            elif op == "*":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 * e2)
            elif op == "/":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 // e2)
            elif op == "%":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 % e2)
            elif op == "<":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 < e2)
            elif op == "<=":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 <= e2)
            elif op == ">":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 > e2)
            elif op == ">=":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(e1 >= e2)
            elif op == "..":
                assert isinstance(e1, int), e1
                assert isinstance(e2, int), e2
                context.stack.append(SetValue(set(range(e1, e2+1))))
            elif op == "/\\" or op == "and":
                assert isinstance(e1, bool), e1
                assert isinstance(e2, bool), e2
                context.stack.append(e1 and e2)
            elif op == "\\/" or op == "or":
                assert isinstance(e1, bool), e1
                assert isinstance(e2, bool), e2
                context.stack.append(e1 or e2)
            else:
                assert False, self
        else:
            assert False, self
        context.pc += 1

class ApplyOp(Op):
    def __init__(self):
        pass

    def __repr__(self):
        return "Apply"

    def eval(self, state, context):
        func = context.stack.pop()
        e = context.stack.pop()
        if isinstance(func, RecordValue):
            context.stack.append(func.d[e])
            context.pc += 1
        else:
            assert isinstance(func, PcValue)
            assert isinstance(state.code[func.pc], RoutineOp), func
            context.stack.append(context.pc + 1)
            context.stack.append(e)
            context.pc = func.pc + 1

class AST:
    pass

class ConstantAST(AST):
    def __init__(self, const):
        self.const = const

    def __repr__(self):
        return str(self.const)

    def compile(self, scope, code):
        code.append(ConstantOp(self.const))

class NameAST(AST):
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return str(self.name)

    def compile(self, scope, code):
        tv = scope.lookup(self.name)
        if tv == None:
            code.append(LoadOp(self.name))
        else:
            (t, v) = tv
            if t == "variable":
                code.append(LoadVarOp(self.name))
            elif t == "constant":
                code.append(ConstantOp(v))
            elif t == "routine":
                (lexeme, file, line, column) = self.name
                code.append(ConstantOp((PcValue(v), file, line, column)))
            else:
                assert False, tv

class SetAST(AST):
    def __init__(self, collection):
        self.collection = collection

    def __repr__(self):
        return str(self.collection)

    def compile(self, scope, code):
        for e in self.collection:
            e.compile(scope, code)
        code.append(SetOp(len(self.collection)))

class RecordAST(AST):
    def __init__(self, record):
        self.record = record

    def __repr__(self):
        return str(self.record)

    def compile(self, scope, code):
        for (k, v) in self.record.items():
            v.compile(scope, code)
            k.compile(scope, code)
        code.append(RecordOp(len(self.record)))

# N-ary operator
class NaryAST(AST):
    def __init__(self, op, args):
        self.op = op
        self.args = args

    def __repr__(self):
        return "NaryOp(" + str(self.op) + ", " + str(self.args) + ")"

    def compile(self, scope, code):
        n = len(self.args)
        for a in range(n):
            self.args[n - a - 1].compile(scope, code)
        code.append(NaryOp(self.op, n))

class ApplyAST(AST):
    def __init__(self, func, arg):
        self.func = func
        self.arg = arg

    def __repr__(self):
        return "Apply(" + str(self.func) + ", " + str(self.arg) + ")"

    def compile(self, scope, code):
        self.arg.compile(scope, code)
        self.func.compile(scope, code)
        code.append(ApplyOp())

class Rule:
    pass

class NaryRule(Rule):
    def __init__(self, closer):
        self.closer = closer

    def parse(self, t):
        (lexeme, file, line, column) = t[0]
        if lexeme in { "-", "not" }:     # unary expression
            op = t[0]
            (ast, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == self.closer, t[0]
            return (NaryAST(op, [ast]), t[1:])
        if lexeme == "tas":
            op = t[0]
            (ast, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == self.closer, t[0]
            return (TasAST(ast), t[1:])
        (ast, t) = ExpressionRule().parse(t)
        (lexeme, file, line, column) = t[0]
        if lexeme == self.closer:
            return (ast, t[1:])
        op = t[0]
        (ast2, t) = ExpressionRule().parse(t[1:])
        (lexeme, file, line, column) = t[0]
        assert lexeme == self.closer, (t[0], self.closer)
        return (NaryAST(op, [ast, ast2]), t[1:])

class RecordRule(Rule):
    def parse(self, t):
        (lexeme, file, line, column) = t[0]
        assert lexeme == "{<", t[0]
        d = {}
        while lexeme != ">}":
            (key, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == ":", t[0]
            (value, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme in { ",", ">}" }
            d[key] = value
        return (RecordAST(d), t[1:])

class BasicExpressionRule(Rule):
    def parse(self, t):
        (lexeme, file, line, column) = t[0]
        if isnumber(lexeme):
            return (ConstantAST((int(lexeme), file, line, column)), t[1:])
        if lexeme == "False":
            return (ConstantAST((False, file, line, column)), t[1:])
        if lexeme == "True":
            return (ConstantAST((True, file, line, column)), t[1:])
        if isname(lexeme):
            return (NameAST(t[0]), t[1:])
        if lexeme[0] == '"':
            d = {}
            for i in range(1, len(lexeme) - 1):
                d[ConstantAST((i, file, line, column + i))] = \
                    ConstantAST((lexeme[i], file, line, column + i))
            return (RecordAST(d), t[1:])
        if lexeme == ".":
            (lexeme, file, line, column) = t[1]
            assert isname(lexeme), t[1]
            return (ConstantAST((lexeme, file, line, column)), t[2:])
        if lexeme == "{":
            s = set()
            while lexeme != "}":
                (next, t) = ExpressionRule().parse(t[1:])
                s.add(next)
                (lexeme, file, line, column) = t[0]
                assert lexeme in { ",", "}" }
            return (SetAST(s), t[1:])
        if lexeme == "{<":
            return RecordRule().parse(t)
        if lexeme == "(" or lexeme == "[":
            closer = ")" if lexeme == "(" else "]"
            (lexeme, file, line, column) = t[1]
            if lexeme == closer:
                return (ConstantAST(
                    (NoValue(), file, line, column)), t[2:])
            return NaryRule(closer).parse(t[1:])
        if lexeme == "&(":
            (ast, t) = LValueRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == ")", t[0]
            return (AddressAST(ast), t[1:])
        if lexeme == "!(":
            (ast, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == ")", t[0]
            return (PointerAST(ast), t[1:])
        if lexeme == "choose(":
            (ast, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == ")", t[0]
            return (ChooseAST(ast), t[1:])
        return (False, t)

class LValueAST(AST):
    def __init__(self, indexes):
        self.indexes = indexes

    def __repr__(self):
        return "LValueRule(" + str(self.indexes) + ")"

class TasAST(AST):
    def __init__(self, expr):
        self.expr = expr

    def __repr__(self):
        return "TAS(" + str(self.expr) + ")"

    def compile(self, scope, code):
        self.expr.compile(scope, code)
        code.append(TasOp())

class PointerAST(AST):
    def __init__(self, expr):
        self.expr = expr

    def __repr__(self):
        return "Pointer(" + str(self.expr) + ")"

    def compile(self, scope, code):
        self.expr.compile(scope, code)
        code.append(PointerOp())

class ChooseAST(AST):
    def __init__(self, expr):
        self.expr = expr

    def __repr__(self):
        return "Choose(" + str(self.expr) + ")"

    def compile(self, scope, code):
        self.expr.compile(scope, code)
        code.append(ChooseOp())

class ExpressionRule(Rule):
    def parse(self, t):
        (ast, t) = BasicExpressionRule().parse(t)
        while t != []:
            (arg, t) = BasicExpressionRule().parse(t)
            if arg == False:
                break
            (ast, t) = (ApplyAST(ast, arg), t)
        return (ast, t)

class AssignmentAST(AST):
    def __init__(self, lv, rv):
        self.lv = lv
        self.rv = rv

    def __repr__(self):
        return "Assign(" + str(self.lv) + ", " + str(self.rv) + ")"

    def compile(self, scope, code):
        self.rv.compile(scope, code)
        n = len(self.lv.indexes)
        for i in range(1, n):
            self.lv.indexes[n - i].compile(scope, code)
        lv = self.lv.indexes[0]
        if isinstance(lv, NameAST):
            tv = scope.lookup(lv.name)
            if tv == None:
                (lexeme, file, line, column) = lv.name
                code.append(ConstantOp(lv.name))
                code.append(StoreOp(n))
            else:
                (t, v) = tv
                if t == "variable":
                    code.append(StoreVarOp(v, n - 1))
                else:
                    assert False, tv
        else:
            assert isinstance(lv, PointerAST), lv
            lv.expr.compile(scope, code)
            code.append(StoreIndOp(n))

class AddressAST(AST):
    def __init__(self, lv):
        self.lv = lv

    def __repr__(self):
        return "Address(" + str(self.lv) + ")"

    def compile(self, scope, code):
        n = len(self.lv.indexes)
        for i in range(1, n):
            self.lv.indexes[n - i].compile(scope, code)
        lv = self.lv.indexes[0]
        if isinstance(lv, NameAST):
            tv = scope.lookup(lv.name)
            assert tv == None, tv   # can't take address of local var
            (lexeme, file, line, column) = lv.name
            code.append(ConstantOp(lv.name))
            code.append(AddressOp(n))
        else:
            assert isinstance(lv, PointerAST), lv
            lv.expr.compile(scope, code)
            code.append(AddressIndOp(n))

class LockAST(AST):
    def __init__(self, lv):
        self.lv = lv

    def __repr__(self):
        return "Lock(" + str(self.lv) + ")"

    def compile(self, scope, code):
        n = len(self.lv.indexes)
        for i in range(1, n):
            self.lv.indexes[n - i].compile(scope, code)
        lv = self.lv.indexes[0]
        assert isinstance(lv, NameAST), lv
        tv = scope.lookup(lv.name)
        assert tv == None, tv       # can't lock local variables
        (lexeme, file, line, column) = lv.name
        code.append(ConstantOp(lv.name))
        code.append(LockOp(n))

class SkipAST(AST):
    def __repr__(self):
        return "Skip"

    def compile(self, scope, code):
        pass

class BlockAST(AST):
    def __init__(self, b):
        self.b = b

    def __repr__(self):
        return "BlockRule(" + str(self.b) + ")"

    def compile(self, scope, code):
        for s in self.b:
            s.compile(scope, code)

class IfAST(AST):
    def __init__(self, alts, stat):
        self.alts = alts        # alternatives
        self.stat = stat        # else statement

    def __repr__(self):
        return "If(" + str(self.alts) + ", " + str(self.what) + ")"

    def compile(self, scope, code):
        jumps = []
        for alt in self.alts:
            (cond, stat) = alt
            cond.compile(scope, code)
            pc = len(code)
            code.append(None)
            stat.compile(scope, code)
            jumps += [len(code)]
            code.append(None)
            code[pc] = JumpFalseOp(len(code))
        if self.stat != None:
            self.stat.compile(scope, code)
        for pc in jumps:
            code[pc] = JumpOp(len(code))

class WhileAST(AST):
    def __init__(self, cond, stat):
        self.cond = cond
        self.stat = stat

    def __repr__(self):
        return "While(" + str(self.cond) + ", " + str(self.stat) + ")"

    def compile(self, scope, code):
        pc1 = len(code)
        self.cond.compile(scope, code)
        pc2 = len(code)
        code.append(None)
        self.stat.compile(scope, code)
        code.append(JumpOp(pc1))
        code[pc2] = JumpFalseOp(len(code))

class AssertAST(AST):
    def __init__(self, cond, expr):
        self.cond = cond
        self.expr = expr

    def __repr__(self):
        return "Assert(" + str(self.cond) + ", " + str(self.expr) + ")"

    def compile(self, scope, code):
        self.cond.compile(scope, code)
        self.expr.compile(scope, code)
        code.append(AssertOp())

class RoutineAST(AST):
    def __init__(self, name, stat):
        self.name = name
        self.stat = stat

    def __repr__(self):
        return "Routine(" + str(self.name) + ", " + str(self.stat) + ")"

    def compile(self, scope, code):
        (lexeme, file, line, column) = self.name
        pc = len(code)
        scope.names[lexeme] = ("routine", pc)
        code.append(None)
        ns = Scope(scope)
        ns.names["self"] = ("variable", self.name)
        code.append(FrameOp())
        self.stat.compile(ns, code)
        code[pc] = RoutineOp(self.name, len(code))
        code.append(ReturnOp())

class CallAST(AST):
    def __init__(self, expr):
        self.expr = expr

    def __repr__(self):
        return "Call(" + str(self.expr) + ")"

    def compile(self, scope, code):
        self.expr.compile(scope, code)
        code.append(PopOp())

class SpawnAST(AST):
    def __init__(self, func, arg):
        self.func = func
        self.arg = arg

    def __repr__(self):
        return "Spawn(" + str(self.func) + ", " + str(self.arg) + ")"

    def compile(self, scope, code):
        self.arg.compile(scope, code)
        self.func.compile(scope, code)
        code.append(SpawnOp())

class LabelAST(AST):
    def __init__(self, label, ast):
        self.label = label
        self.ast = ast

    def __repr__(self):
        return "Label(" + str(self.label) + ", " + str(self.ast) + ")"

    def compile(self, scope, code):
        code.append(LabelOp(self.label))
        self.ast.compile(scope, code)

class VarAST(AST):
    def __init__(self, var, expr):
        self.var = var
        self.expr = expr

    def __repr__(self):
        return "Var(" + str(self.var) + ", " + str(self.expr) + ")"

    def compile(self, scope, code):
        self.expr.compile(scope, code)
        (lexeme, file, line, column) = self.var
        scope.names[lexeme] = ("variable", self.var)
        code.append(StoreVarOp(self.var, 0))

class ConstAST(AST):
    def __init__(self, const, value):
        self.const = const
        self.value = value

    def __repr__(self):
        return "Const(" + str(self.const) + ", " + str(self.value) + ")"

    def compile(self, scope, code):
        (lexeme, file, line, column) = self.const
        scope.names[lexeme] = ("constant", self.value)

class LValueRule(Rule):
    def parse(self, t):
        (name, file, line, column) = t[0]
        if isname(name):
            indexes = [NameAST(t[0])]
        else:
            assert name == "!(", t[0]
            (ast, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == ")"
            indexes = [PointerAST(ast)]
        t = t[1:]
        while t != []:
            (index, t) = BasicExpressionRule().parse(t)
            if index == False:
                break
            indexes.append(index)
        return (LValueAST(indexes), t)

class AssignmentRule(Rule):
    def parse(self, t):
        (lv, t) = LValueRule().parse(t)
        (lexeme, file, line, column) = t[0]
        assert lexeme == ":=", t[0]
        (rv, t) = NaryRule(";").parse(t[1:])
        return (AssignmentAST(lv, rv), t)

class StatListRule(Rule):
    def __init__(self, delim):
        self.delim = delim

    def parse(self, t):
        b = []
        (lexeme, file, line, column) = t[0]
        while lexeme not in self.delim:
            (ast, t) = StatementRule().parse(t)
            b.append(ast)
            if t == [] and self.delim == set():
                break
            (lexeme, file, line, column) = t[0]
        return (BlockAST(b), t)

class BlockRule(Rule):
    def __init__(self, delim):
        self.delim = delim

    def parse(self, t):
        (lexeme, file, line, column) = t[0]
        assert lexeme == ":", t[0]
        return StatListRule(self.delim).parse(t[1:])

class StatementRule(Rule):
    def parse(self, t):
        (lexeme, file, line, column) = t[0]
        if lexeme == "@":
            label = t[1]
            (lexeme, file, line, column) = t[1]
            assert isname(lexeme), t[1]
            (lexeme, file, line, column) = t[2]
            assert lexeme == ":", t[2]
            (ast, t) = StatementRule().parse(t[3:])
            return (LabelAST(label, ast), t)
        if lexeme == "var":
            var = t[1]
            (lexeme, file, line, column) = t[1]
            assert isname(lexeme), t[1]
            (lexeme, file, line, column) = t[2]
            assert lexeme == "=", t[2]
            (ast, t) = NaryRule(";").parse(t[3:])
            return (VarAST(var, ast), t)
        if lexeme == "const":
            const = t[1]
            (lexeme, file, line, column) = t[1]
            assert isname(lexeme), t[1]
            (lexeme, file, line, column) = t[2]
            assert lexeme == "=", t[2]
            (ast, t) = NaryRule(";").parse(t[3:])
            assert isinstance(ast, ConstantAST), ast
            return (ConstAST(const, ast.const), t)
        if lexeme == "if":
            alts = []
            while True:
                (cond, t) = NaryRule(":").parse(t[1:])
                (stat, t) = StatListRule({ "else", "elif", "end" }).parse(t)
                alts += [(cond, stat)]
                (lexeme, file, line, column) = t[0]
                if lexeme in { "else", "end" }:
                    break
                assert lexeme == "elif", t[0]
                t = t[1:]
            if lexeme == "else":
                (stat, t) = BlockRule({"end"}).parse(t[1:])
                (lexeme, file, line, column) = t[0]
            else:
                stat = None
            assert lexeme == "end", t[0]
            (lexeme, file, line, column) = t[1]
            assert lexeme == "if", t[1]
            return (IfAST(alts, stat), t[2:])
        if lexeme == "while":
            (cond, t) = NaryRule(":").parse(t[1:])
            (stat, t) = StatListRule({"end"}).parse(t)
            (lexeme, file, line, column) = t[1]
            assert lexeme == "while", t[1]
            return (WhileAST(cond, stat), t[2:])
        if lexeme == "routine":
            name = t[1]
            (lexeme, file, line, column) = name
            assert isname(lexeme), lv
            (stat, t) = BlockRule({"end"}).parse(t[2:])
            (lexeme, file, line, column) = t[1]
            assert lexeme == "routine", t[1]
            return (RoutineAST(name, stat), t[2:])
        if lexeme == "call":
            (expr, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == ";", t[0]
            return (CallAST(expr), t[1:])
        if lexeme == "spawn":
            (func, t) = ExpressionRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme in [",", ";"], t[0]
            if lexeme == ",":
                (expr, t) = NaryRule(";").parse(t[1:])
                return (SpawnAST(func, expr), t)
            else:
                return (SpawnAST(func, ConstantAST(
                    (NoValue(), file, line, column))), t[2:])
        if lexeme == "lock":
            (lv, t) = LValueRule().parse(t[1:])
            (lexeme, file, line, column) = t[0]
            assert lexeme == ";", t[0]
            return (LockAST(lv), t[1:])
        if lexeme == "skip":
            return (SkipAST(), t[1:])
        if lexeme == "assert":
            (cond, t) = NaryRule(":").parse(t[1:])
            (expr, t) = NaryRule(";").parse(t)
            return (AssertAST(cond, expr), t)
        return AssignmentRule().parse(t)

class Context:
    def __init__(self, name, id, pc, end):
        self.name = name
        self.id = id
        self.pc = pc
        self.end = end
        self.stack = []
        self.vars = RecordValue({})

    def __repr__(self):
        return "Context(" + str(self.name) + ", " + str(self.id) + ", " + str(self.stack) + ", " + str(self.vars) + ")"

    def __hash__(self):
        h = self.name.__hash__() ^ self.id ^ self.pc ^ self.end ^ self.vars.__hash__()
        for v in self.stack:
            h ^= v.__hash__()
        return h

    def __eq__(self, other):
        if not isinstance(other, Context):
            return False
        if self.name != other.name:
            return False
        if self.id != other.id:
            return False
        if self.pc != other.pc:
            return False
        assert self.end == other.end
        return self.stack == other.stack and self.vars == other.vars

    def copy(self):
        c = Context(self.name, self.id, self.pc, self.end)
        c.stack = self.stack.copy()
        c.vars = self.vars
        return c

    def get(self, var):
        return self.vars.d[var]

    def iget(self, indexes):
        v = self.vars
        while indexes != []:
            v = v.d[indexes[0]]
            indexes = indexes[1:]
        return v

    def update(self, record, indexes, val):
        if len(indexes) > 1:
            v = self.update(record.d[indexes[0]], indexes[1:], val)
        else:
            v = val
        d = record.d.copy()
        d[indexes[0]] = v
        return RecordValue(d)

    def set(self, indexes, val):
        self.vars = self.update(self.vars, indexes, val)

class State:
    def __init__(self, code):
        self.code = code
        self.vars = RecordValue({})
        name = "__main__"
        id = 0
        self.contexts = { (name, id) : Context(name, id, 0, len(code)) }

    def __repr__(self):
        return "State(" + str(self.vars) + ", " + str(self.contexts) + ")"

    def __hash__(self):
        h = self.vars.__hash__()
        for c in self.contexts.values():
            h ^= c.__hash__()
        return h

    def __eq__(self, other):
        if not isinstance(other, State):
            return False
        assert self.code == other.code
        if self.vars != other.vars:
            return False
        if self.contexts.keys() != other.contexts.keys():
            return False
        for (k, v) in self.contexts.items():
            if v != other.contexts[k]:
                return False
        return True

    def copy(self):
        s = State(self.code)
        s.vars = self.vars      # no need to copy as store operations do it
        s.contexts = {}
        for (k, v) in self.contexts.items():
            s.contexts[k] = v.copy()
        return s

    def get(self, var):
        return self.vars.d[var]

    def iget(self, indexes):
        v = self.vars
        while indexes != []:
            v = v.d[indexes[0]]
            indexes = indexes[1:]
        return v

    def update(self, record, indexes, val):
        if len(indexes) > 1:
            v = self.update(record.d[indexes[0]], indexes[1:], val)
        else:
            v = val
        d = record.d.copy()
        d[indexes[0]] = v
        return RecordValue(d)

    def set(self, indexes, val):
        self.vars = self.update(self.vars, indexes, val)

class Node:
    def __init__(self, parent, ctx, choice, steps, len):
        self.parent = parent
        self.ctx = ctx
        self.choice = choice
        self.steps = steps
        self.len = len
        self.edges = []

def print_path(visited, state):
    if state != None:
        node = visited[state]
        print_path(visited, node.parent)
        print(node.ctx, node.steps, state.vars)

def print_shortest(visited, bad):
    best_state = None
    best_len = 0
    for s in bad:
        node = visited[s]
        if best_state == None or node.len < best_len:
            best_state = s
            best_len = node.len
    print_path(visited, best_state)

class Scope:
    def __init__(self, parent):
        self.parent = parent
        self.names = {}

    def lookup(self, name):
        (lexeme, file, line, column) = name
        tv = self.names.get(lexeme)
        if tv != None:
            return tv
        ancestor = self.parent
        while ancestor != None:
            tv = ancestor.names.get(lexeme)
            if tv != None:
                (t, v) = tv
                if t != "variable":
                    return tv
                return None
            ancestor = ancestor.parent
        return None

# These operations cause global state changes
globops = [
    LabelOp, LoadOp, LockOp, LockOp, SpawnOp, StoreOp, TasOp
]

def onestep(state, k, choice, visited, todo, node, infloop):
    sc = state.copy()
    ctx = sc.contexts[k]
    if choice == None:
        op = sc.code[ctx.pc]
        if isinstance(op, LockOp):
            assert op.n == 1, op        # TODO.  Generalize
            top = ctx.stack[-1]
            v = sc.iget([top])
            assert isinstance(v, bool)
            if v:
                return False
        steps = []
    else:
        steps = [ctx.pc]
        ctx.stack[-1] = choice
        ctx.pc += 1

    localStates = { sc.copy() }
    while True:
        # execute one step
        steps.append(ctx.pc)
        # print("PC", ctx.pc, sc.code[ctx.pc])
        sc.code[ctx.pc].eval(sc, ctx)

        # if we reached the end, remove the context
        if ctx.pc == ctx.end:
            del sc.contexts[k]
            break

        # if we're about to do a state change, let other processes
        # go first assuming there are other processes
        if type(sc.code[ctx.pc]) in globops and len(sc.contexts) > 1:
            break
        if isinstance(sc.code[ctx.pc], ChooseOp):
            break

        # Detect infinite loops
        if sc in localStates:
            infloop.add(sc.copy())
            break
        localStates.add(sc.copy())

    next = visited.get(sc)
    if next == None:
        next = Node(state, k, choice, steps, node.len + 1)
        visited[sc] = next
        todo.append(sc)
    node.edges.append(sc)
    return True

def run(invariant, pcs):
    all = ""
    for line in sys.stdin:
       all += line
    tokens = lexer(all, "<stdin>")
    (ast, rem) = StatListRule(set()).parse(tokens)
    code = []
    ast.compile(Scope(None), code)

    for pc in range(len(code)):
        print(pc, code[pc])

    # Initial state
    state = State(code)

    # For traversing Kripke graph
    visited = { state: Node(None, None, None, None, 0) }
    todo = [state]
    bad = set()
    infloop = set()

    cnt = 0
    while todo != []:
        cnt += 1
        state = todo[0]
        todo = todo[1:]
        node = visited[state]
        print(" ", cnt, "#states =", len(visited.keys()), "diameter =", node.len, "queue =", len(todo), end="     \r")

        if not invariant(state):
            bad.add(state)

        deadlock = len(state.contexts.items()) > 0
        for (k, c) in state.contexts.items():
            if c.pc < c.end and isinstance(code[c.pc], ChooseOp):
                choices = c.stack[-1]
                assert isinstance(choices, SetValue), choices
                assert len(choices.s) > 0
                for choice in choices.s:
                    if onestep(state, k, choice, visited, todo, node, infloop):
                        deadlock = False
            elif onestep(state, k, None, visited, todo, node, infloop):
                deadlock = False
        assert not deadlock
    print()

    # See if there has been a safety violation
    if len(bad) > 0:
        print("==== Safety violation ====")
        print_shortest(visited, bad)

    # See if there are processes stuck in infinite loops without accessing
    # shared state
    if len(infloop) > 0:
        print("==== Infinite Loop ====")
        print_shortest(visited, infloop)

    # See if there are livelocked states (states from which some process
    # cannot reach the reader or writer critical section)
    bad = set()
    for (p, cs) in pcs:
        # First collect all the states in which the process is in the
        # critical region
        good = set()
        for s in visited.keys():
            # for ctx in s.contexts.values():
            ctx = s.contexts.get(p)
            if ctx == None or ctx.pc == 0:
                continue
            op = s.code[ctx.pc]
            if isinstance(op, LabelOp) and op.label[0] == cs:
                good.add(s)
        progress = True
        while progress:
            progress = False
            for (s, node) in visited.items():
                if s not in good:
                    for reachable in node.edges:
                        if reachable in good:
                            progress = True
                            good.add(s)
                            break
        livelocked = set(visited.keys()).difference(good)
        bad = bad.union(livelocked)
    if len(bad) > 0:
        print("==== Livelock ====")
        print_shortest(visited, bad)
        # for (s, n) in visited.items():
        #     print(">>>>>>>>")
        #     print_path(visited, s)

    return visited
