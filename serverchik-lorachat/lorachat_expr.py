"""
Крошечный язык выражений для виджетов датчиков.

Зачем не JavaScript и не eval. В исходной задумке скрипт виджета
должен был ехать вместе с пакетом датчика — «все, кто получил пакет,
ставят виджет и он по скрипту показывает разные вещи». Так делать
нельзя: пакет приходит по радио от кого угодно, подписи у него нет, а
исполняемый код из такого пакета выполнился бы у каждого, кто его
услышал. Это удалённое исполнение кода, причём с эфира, где отправителя
не проверить.

Поэтому здесь два решения:

  • выражения хранятся **на сервере** и раздаются по вебу, где
    отправитель известен и права проверяемы, а не приходят из эфира;
  • сам язык не умеет ничего, кроме арифметики над показаниями:
    ни вызовов, ни атрибутов, ни импортов, ни циклов.

Что можно:  t * 1.8 + 32
            rain > 0 ? "дождь" : "сухо"
            round(v / 10, 1)
            min(a, b) + max(c, 0)
            wet and not frozen

Разбор идёт штатным ast-модулем Python, но пропускаются только
разрешённые узлы. Всё остальное — ошибка на этапе компиляции, то есть
в момент, когда человек сохраняет виджет, а не когда датчик прислал
неудачное число.
"""

import ast
import math
import operator

MAX_LEN = 500          # выражение длиннее — почти наверняка не выражение
MAX_NODES = 120        # защита от «(((((…)))))» на сто уровней


class ExprError(ValueError):
    pass


# Разрешённые операции. Возведение в степень намеренно отсутствует:
# 9**9**9 повесит процесс на ровном месте.
_BINOPS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
}
_UNARYOPS = {ast.UAdd: operator.pos, ast.USub: operator.neg, ast.Not: operator.not_}
_CMPOPS = {
    ast.Eq: operator.eq, ast.NotEq: operator.ne,
    ast.Lt: operator.lt, ast.LtE: operator.le,
    ast.Gt: operator.gt, ast.GtE: operator.ge,
}


def _round(x, digits=0):
    return round(float(x), int(digits))


def _clamp(x, lo, hi):
    return max(float(lo), min(float(hi), float(x)))


_FUNCS = {
    'round': _round,
    'abs': lambda x: abs(float(x)),
    'min': lambda *a: min(a),
    'max': lambda *a: max(a),
    'clamp': _clamp,
    'int': lambda x: int(float(x)),
    'floor': lambda x: math.floor(float(x)),
    'ceil': lambda x: math.ceil(float(x)),
    'sqrt': lambda x: math.sqrt(max(0.0, float(x))),
}

_ALLOWED_NODES = (
    ast.Expression, ast.Constant, ast.Name, ast.Load,
    ast.BinOp, ast.UnaryOp, ast.BoolOp, ast.Compare, ast.IfExp, ast.Call,
    ast.And, ast.Or,
) + tuple(_BINOPS) + tuple(_UNARYOPS) + tuple(_CMPOPS)


class Expr:
    """Скомпилированное выражение. Компилируется один раз, считается много."""

    __slots__ = ('source', '_tree', 'names')

    def __init__(self, source):
        if not isinstance(source, str):
            raise ExprError('выражение должно быть строкой')
        source = source.strip()
        if not source:
            raise ExprError('пустое выражение')
        if len(source) > MAX_LEN:
            raise ExprError('выражение длиннее %d символов' % MAX_LEN)
        try:
            tree = ast.parse(source, mode='eval')
        except SyntaxError as e:
            raise ExprError('не разбирается: %s' % e.msg) from None

        names = set()
        count = 0
        for node in ast.walk(tree):
            count += 1
            if count > MAX_NODES:
                raise ExprError('выражение слишком сложное')
            if not isinstance(node, _ALLOWED_NODES):
                raise ExprError('нельзя использовать %s'
                                % type(node).__name__)
            if isinstance(node, ast.Call):
                # Вызывать можно только по имени и только из белого
                # списка: obj.method() закрыт вместе с атрибутами.
                if not isinstance(node.func, ast.Name):
                    raise ExprError('вызов возможен только по имени функции')
                if node.func.id not in _FUNCS:
                    raise ExprError('нет функции %s' % node.func.id)
                if node.keywords:
                    raise ExprError('именованные аргументы не поддержаны')
            elif isinstance(node, ast.Name):
                if node.id not in _FUNCS:
                    names.add(node.id)
            elif isinstance(node, ast.Constant):
                if not isinstance(node.value, (int, float, str, bool)):
                    raise ExprError('недопустимая константа')

        self.source = source
        self._tree = tree
        self.names = names

    def eval(self, values):
        """Посчитать. values — {имя поля: число}.

        Ошибка вычисления (нет значения, деление на ноль) — не
        исключение наружу, а None: датчик мог просто не прислать поле,
        и падать из-за этого целой странице незачем.
        """
        try:
            return self._eval(self._tree.body, values)
        except ExprError:
            raise
        except Exception:
            return None

    def _eval(self, node, env):
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, ast.Name):
            if node.id not in env:
                raise KeyError(node.id)
            return env[node.id]
        if isinstance(node, ast.BinOp):
            return _BINOPS[type(node.op)](self._eval(node.left, env),
                                          self._eval(node.right, env))
        if isinstance(node, ast.UnaryOp):
            return _UNARYOPS[type(node.op)](self._eval(node.operand, env))
        if isinstance(node, ast.BoolOp):
            vals = [self._eval(v, env) for v in node.values]
            if isinstance(node.op, ast.And):
                out = True
                for v in vals:
                    if not v:
                        return v
                    out = v
                return out
            for v in vals:
                if v:
                    return v
            return vals[-1] if vals else False
        if isinstance(node, ast.Compare):
            left = self._eval(node.left, env)
            for op, comp in zip(node.ops, node.comparators):
                right = self._eval(comp, env)
                if not _CMPOPS[type(op)](left, right):
                    return False
                left = right
            return True
        if isinstance(node, ast.IfExp):
            return (self._eval(node.body, env) if self._eval(node.test, env)
                    else self._eval(node.orelse, env))
        if isinstance(node, ast.Call):
            args = [self._eval(a, env) for a in node.args]
            return _FUNCS[node.func.id](*args)
        raise ExprError('неожиданный узел %s' % type(node).__name__)


def compile_expr(source):
    return Expr(source)


def safe_eval(source, values):
    """Разовое вычисление. Для виджетов лучше держать Expr скомпилированным."""
    return Expr(source).eval(values)
