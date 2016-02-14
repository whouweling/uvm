#!/usr/bin/python

# Quick & dirty compiler to test the uvm VM

import re
import sys

class ParseError(Exception):
    pass

TOKEN_NUMBER = "num"
TOKEN_OP = "op"
TOKEN_VAR = "var"
TOKEN_CONST = "const"
TOKEN_STRING = "string"
TOKEN_FUNC = "func"

TYPE_INT = "int"
TYPE_CHAR = "char"

class Ref:
    def __init__(self, name):
        self.name = name

class Global(Ref):
    pass

class Indexed(Ref):
    pass


def cstrip(source, count):
    return [line[count:] for line in source]

class Parser:

    statements = {
        "include": "^include (?P<name>[a-z]+)$",
        "call": "(?P<name>[a-z]+)\((?P<param>.*)\)$",
        "func": ":(?P<name>[a-z]+)\((?P<header>.*)\)",
        "def": "(?P<type>int|char)\s(?P<name>[a-z]+)(\[(?P<size>[0-9]+)\])?$",
        "assign": "(?P<var>[a-z0-9\[\]]+)\s=\s(?P<expr>.+)",
        "ret": "ret\s(?P<expr>.+)",
        "if": "^if\s(?P<expr>.+)",
        "while": "^while\s(?P<expr>.+)",
    }

    tokenizer = {
        TOKEN_NUMBER: r'^[0-9]+$',
        TOKEN_OP: r'^(\+|-|==|!=)$',
        TOKEN_CONST: r'^.[a-z0-9]+$',
        TOKEN_VAR: r'^[a-z][a-z0-9]*$',
        TOKEN_STRING: r'^\"(.+)\"$',
        TOKEN_FUNC: "(?P<name>[a-z]+)\((?P<param>.*)\)$"
    }

    indexed = re.compile("^(?P<var>[a-z][a-z0-9]*)\[(?P<index>[0-9]+)\]$")

    def __init__(self, context=None, depth=0, offset=0):

        self.result = []
        self.text = {}
        self.text_index = 0
        self.depth = depth

        self.delayed_skip = None
        self.delayed_instr = None
        self.offset = offset

        if not context:
            self.context = {}
        else:
            self.context = context

        for macro, regex in self.statements.items():
            self.statements[macro] = re.compile(regex)

        for type, regex in self.tokenizer.items():
            self.tokenizer[type] = re.compile(regex)

    def handle_include(self, name):
        self.out("include", name)

    def store(self, name):


        try:
            if self.context[name]['type'] == TYPE_INT:
                return self.out("istore")
            if self.context[name]['type'] == TYPE_CHAR:
                return self.out("store")
        except KeyError:
            raise ParseError("unknown variable {}".format(name))


    def load(self, name):

        try:

            # Indexed var's are passed by ref.
            if self.context[name]['size'] > 1:
                return

            if self.context[name]['type'] == TYPE_INT:
                self.out("iload")
            if self.context[name]['type'] == TYPE_CHAR:
                self.out("load")
        except KeyError:
            raise ParseError("unknown variable {}".format(name))


    def handle_expr(self, expr):

        tokens = re.findall(r'''(?:[^;'" ]+|'(?:[^']|\\.)*'|"(?:[^']|\\.)*")+''', expr)

        ops = []

        for token in tokens:

            for type, regex in self.tokenizer.items():

                match = regex.match(token)

                if match:

                    if type == TOKEN_NUMBER:
                        self.out("push", token)

                    if type == TOKEN_OP:
                        ops.append(token)

                    if type in TOKEN_CONST:
                        self.out("push", token)

                    if type in TOKEN_VAR:
                        self.out("push", "%{}".format(token))
                        self.load(token)

                    if type in TOKEN_STRING:

                        if token not in self.text:
                            self.text_index += 1
                            key = "t{}{}".format(self.depth,
                                                  len(self.text))
                            self.text[token] = key

                        self.out("push", ".{}".format(self.text[token]))

                    if type in TOKEN_FUNC:
                        self.handle_call(**match.groupdict())

                    break
            else:
                raise ParseError("unknown expression '{}'".format(expr))

        for op in ops:

            if op == "+":
                self.out("add")

            if op == "==":
                self.out("eq")

            if op == "!=":
                self.out("eq")
                self.out("not")

    def handle_ret(self, expr):
        self.handle_expr(expr)
        self.out("swap")
        self.out("ret")

    def handle_func(self, name, header):

        self.out(":{}".format(name))

        for par in header.split(","):

            if not par:
                continue

            par = par.lstrip()

            (type, name) = par.split(" ")

            self.handle_def(type=type,
                            name=name[1:])

            self.out("swap")
            self.out("push", name)
            self.store(name)

            #raise Exception(par)

    def handle_if(self, expr):
        self.handle_expr(expr)
        self.delayed_skip = True
        #raise Exception("if")

    def handle_while(self, expr):
        label = ":loop{}".format(self.depth)
        self.out(label)
        self.handle_expr(expr)
        self.delayed_skip = True
        self.delayed_instr = ("jump", label)

    def handle_call(self, name, param):

        for param in param.split(","):
            self.handle_expr(param)

        self.out("call", ":{}".format(name))

    def handle_def(self, type, name, size=1):

        if not size:
            size = 1

        if type == TYPE_INT:
            self.out("%{name}".format(name=name), 4*size)
        elif type == TYPE_CHAR:
            self.out("%{name}".format(name=name), size)
        else:
            raise ParseError("unknown type '{}'".format(type))

        self.context[name] = {
            "type": type,
            "size": size
        }

    def handle_var(self, var):

        matches = self.indexed.match(var)
        if matches:
            (var, index) = matches.groups()
            self.handle_expr(index)

        self.out("push", "%{var}".format(var=var))

        if matches:
            self.out("add")

        return var

    def handle_assign(self, var, expr):

        self.handle_expr(expr)

        var = self.handle_var(var)
        self.store(var)


    def out(self, instr, parameter=None):
        l = [instr]
        if parameter:
            l.append(str(parameter))
        self.result.append(" ".join(l))

    def handle_sub(self, block, first_incr, index):

        # TODO: Yuk, dit kan mooier

        if not block:
            return

        text, sub_block = Parser(context=self.context,
                                 depth=self.depth+1,
                                 offset=self.offset+index).parse(cstrip(block, first_incr))

        if self.delayed_skip:
            skip = len(sub_block)
            if self.delayed_instr:
                skip += 1
            self.out("skz", skip)
            self.delayed_skip = None

        self.result.extend(sub_block)
        self.text.update(text)

        if self.delayed_instr:
            self.out(*self.delayed_instr)
            self.delayed_instr = None


    def parse(self, source):

        block = []
        indent = 0
        first_incr = None

        for index, line in enumerate(source):

            if not line:
                continue

            c_indent = 0
            for c in line:
                if c != " ":
                    break
                c_indent += 1


            if c_indent == indent:

                self.handle_sub(block, first_incr, index)
                block = []

                line = line.lstrip()

                for macro, regex in self.statements.items():

                    matches = regex.match(line)
                    if matches:
                        try:
                            parameters = matches.groupdict()
                            getattr(self, "handle_{}".format(macro))(**parameters)
                        except ParseError as e:
                            raise ParseError("error on line {}: {} \n{}".format(self.offset+index, e, line))
                        break
                else:
                    raise ParseError("syntax error on line {}:\n{}".format(self.offset+index, line))
            else:
                if not first_incr:
                    first_incr = c_indent
                block.append(line)

        self.handle_sub(block, first_incr, index)
        return (self.text, self.result)


if __name__ == "__main__":

    (_, input_filename) = sys.argv

    with open(input_filename, "r") as source:

        text, result = Parser().parse([line[:-1] for line in source])

        result.append("halt")

        # output the text segment
        for value, key in text.items():
            print(".{} {}".format(key, value[1:-1]))

        # output the program
        print("\n".join(result))
