# CinderX HIR 指令完整指南

> 适用范围：本仓库 `cinderx/Jit/hir/` 下的 HIR（High-level Intermediate Representation，高层中间表示）。
> 指令全集以 `cinderx/Jit/hir/hir_ops.h` 中的 `FOREACH_OPCODE` 宏为准，当前共 **183 条** opcode；各指令的类定义、操作数与注释位于 `cinderx/Jit/hir/hir.h`（约 4200 行）。

## 1. HIR 是什么

HIR 是 CinderX JIT 的机器无关高层中间表示，介于 CPython 字节码与 LIR 之间。它在 `hir.h` 文件头注释中明确了三条设计目标：

1. **贴近 Python**：保持高层抽象，使类型特化、null 检查消除等优化更容易进行；
2. **尽可能显式**：字节码中隐式的逻辑（引用计数、null 检查、异常检查）在 HIR 中全部显式化为独立指令，从而可以被优化掉；
3. **易于下降（lowering）**：可以机械化地翻译为 LIR（进而生成机器码）。

HIR 中的函数是一个由基本块（`BasicBlock`）组成控制流图（CFG），每个基本块以终结指令（terminator）结尾。指令经 SSA 变换后处于 SSA 形式，值由 `Register` 表示。

## 2. HIR 在编译管线中的位置

```
PyCodeObject 字节码
   │  preload（Jit/hir/preload.cpp，预取全局名等）
   ▼
HIR 生成（Jit/hir/builder.cpp，HIRBuilder 逐条翻译字节码）
   │  SSAify + 类型流动（Jit/hir/ssa.cpp）
   ▼
HIR 优化 pass 序列（Jit/compiler.cpp 的 Compiler::runPasses）
   ▼
LIR 生成（Jit/lir/generator.cpp，LIRGenerator::TranslateOneBasicBlock 按 opcode switch 下降）
   ▼
LIR pass + 寄存器分配（Jit/lir/）→ asmjit 机器码（Jit/codegen/）
```

`Compiler::runPasses`（`Jit/compiler.cpp`）中 HIR pass 的运行顺序概要：

| 顺序 | Pass（源文件） | 作用 |
|---|---|---|
| 1 | SSAify（ssa.cpp） | 转 SSA 形式并在 SSA 上流动类型 |
| 2 | Simplify（simplify.cpp） | 常量折叠、强度削减、窥孔等通用化简 |
| 3 | FloatAccumulatorPromotion | 循环中浮点累加器提升为 CDouble |
| 4 | DynamicComparisonElimination | 消除可静态判定的动态比较 |
| 5 | GuardTypeRemoval（guard_removal.cpp） | 删除被类型信息证明恒真的 GuardType |
| 6 | PhiElimination（phi_elimination.cpp） | 消除平凡/冗余 Phi |
| 7 | InlineFunctionCalls（inliner.cpp） | HIR 级函数内联（生成 Begin/EndInlinedFunction） |
| 8 | BuiltinLoadMethodElimination | 消除内建类型的 LoadMethod |
| 9 | PrimitiveUnboxCSE | 拆箱公共子表达式消除 |
| 9a | （仅 ARM）FloatComparisonSimplification、PrimitiveBoxRemat | 浮点比较化简、装箱再物化 |
| 10 | CleanCFG → DeadCodeElimination → CleanCFG | 清理 CFG 与死代码 |
| 11 | TreeIterStateMachinePass | 把递归 yield-from 树遍历改写为显式状态机（TreeIter 系列指令） |
| 12 | RefcountInsertion（refcount_insertion.md） | 插入 Incref/Decref/XIncref/XDecref/BatchDecref，**正确性必需** |
| 13 | InsertUpdatePrevInstr | 插入 UpdatePrevInstr（维护帧的 prev_instr） |

## 3. 核心概念

### 3.1 指令、操作数与输出

- 所有 HIR 指令继承自 `jit::hir::Instr`（hir.h:129）。指令实例是变长的：操作数（`Register*` 数组）存储在对象**之前**，因此必须通过各子类的 `create()` 工厂分配。
- 每条指令最多一个输出（`output()`）；无输出的指令用于副作用（存储、分支、引用计数等）。
- 指令通过 `INSTR_CLASS(name, (操作数类型列表), HasOutput?, Operands<N>?, 基类?)` 宏族定义；`DEFINE_SIMPLE_INSTR` 用于无自定义成员的简单指令。
- 每条指令记录其来源字节码偏移（`bytecodeOffset()`），dump 时显示为 `CurInstrOffset`。

### 3.2 可 deopt 指令与 FrameState

继承 `DeoptBase`（hir.h:289）的指令在运行时条件不满足时可以**去优化（deopt）**：带着附加的 `FrameState`（重建解释器帧所需的全部元数据：locals、栈、code、字节码偏移等）回到解释器继续执行。dump 中表现为指令后跟 `{ LiveValues<...> ... FrameState {...} }` 注释块。

`DeoptBase` 的常见子类：

- `CheckBase` / `CheckBaseWithName`：各类"检查失败即 deopt"指令的基类（CheckExc、CheckVar 等）；
- `DeoptBaseWithNameIdx`：携带 `co_names` 索引的 deopt 指令（LoadAttr/StoreAttr/LoadGlobal 等）。

deopt 机制详见 `cinderx/Jit/deoptimization.md`。

### 3.3 类型系统

HIR 是带类型的 IR。类型 `jit::hir::Type`（`Jit/hir/type.h`，文档 `Jit/hir/type.md`）分为两大类：

- **装箱类型（boxed）**：`TObject`、`TOptObject`（可为 NULL）、`TLongExact`、`TFloatExact`、`TUnicodeExact`、`TTupleExact`、`TList`、`TDict`、`TType`、`TFunc`、`TCode` 等，对应 `PyObject*`；
- **原始类型（primitive）**：`TCInt`/`TCInt64`/`TCInt32`、`TCBool`、`TCDouble`、`TCUInt64`、`TCPtr` 等，对应未装箱的 C 值。

操作数类型还可以是 `Constraint`（hir.h:80），表示"必须能匹配为某类类型"的宽松约束，如 `kMatchAllAsCInt`、`kListOrChkList`。

### 3.4 终结指令

基本块必须以下列指令之一结尾：`Branch`、`CondBranch`（及其变体）、`Return`。

## 4. 如何 dump 与阅读 HIR

### 4.1 环境变量（等价 `-X` 选项）

在 `Jit/pyjit.cpp` 的 `initFlagProcessor()` 中注册，常用项：

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITDUMPHIR` | dump 初始（未优化）HIR |
| `PYTHONJITDUMPHIRPASSES` | 每个 HIR pass 前后各 dump 一次 |
| `PYTHONJITDUMPFINALHIR` | dump 最终优化后的 HIR |
| `PYTHONJITDUMPHIRSTATS` | dump HIR 指令统计 |
| `PYTHONJITLOGFILE=path` | 把 JIT 日志（含 dump）写入文件 |
| `PYTHONJITALL=1` / `PYTHONJITLISTFILE` | 控制哪些函数被编译（全量 / 名单文件） |
| `PYTHONJITDEBUGREFCOUNT` / `PYTHONJITDEBUGREGALLOC` / `PYTHONJITDEBUGINLINER` | 各子系统调试输出 |

示例：

```bash
python -X jit-all -X jit-dump-final-hir demo.py
PYTHONJITALL=1 PYTHONJITDUMPFINALHIR=1 PYTHONJITLOGFILE=jit.log python demo.py
```

### 4.2 输出格式

打印器为 `Jit/hir/printer.cpp`。格式：`输出寄存器:类型 = OpName<编译期属性> 操作数...`，基本块为 `bb N (preds ...) { ... }`。示例（摘自 `Jit/guide.md`）：

```
fun __main__:f {
  bb 0 {
    v6:Object = LoadArg<0; "a">
    v10:CInt32 = IsTruthy v8 {
      LiveValues<3> b:v6 b:v7 b:v8
      CurInstrOffset 2
      Locals<3> v6 v7 v8
    }
    CondBranch<1, 2> v10
  }
  bb 1 (preds 0) {
    v13:Object = BinaryOp<Add> v6 v7 { ... }
    Return v13
  }
}
```

- `v6:Object`：SSA 寄存器编号与其类型；`b:v6` 表示该值是借用的引用；
- `<...>` 内是该指令的编译期属性（如 `BinaryOp<Add>` 的运算种类、`CondBranch<1, 2>` 的真/假目标块号）；
- 可 deopt 指令后的 `{ ... }` 是 deopt 元数据（LiveValues、CurInstrOffset、FrameState）。

## 5. 全量指令参考（183 条）

指令条目格式：**名称** `(操作数) → 输出`，标注 `deopt` 表示继承 DeoptBase（可去优化）。"来源"指通常生成该指令的字节码或 pass。

### 5.1 常量、寄存器与类型操作（13 条）

- **`Assign`** `(src) → dst`：寄存器拷贝。SSA 化与各 pass 中大量使用的 passthrough 指令，LIR 阶段会被 copy-propagation 消除。
- **`LoadConst`** `(type) → dst`：把编译期常量（以 `Type` 表示的单值：None、True/False、小整数、已知对象等）装入寄存器。来源：LOAD_CONST 等。
- **`BitCast`** `(src) → dst`：带新类型的寄存器拷贝，**不做任何运行时检查**，用于优化器内部改写类型。
- **`RefineType`** `(src) → dst`：输出类型为给定类型与输入类型的交集。仅收窄类型信息、不生成代码（passthrough），用于把分支信息回填给后续使用点。
- **`GuardType`** `(src) → dst` ·deopt：运行时验证 src 的类型属于 target，失败则 deopt；成功时输出带 refined type 的拷贝。GuardTypeRemoval pass 会删除被证明恒真的实例。
- **`GuardIs`** `(src) → dst` ·deopt：验证 src 与编译期对象 target 是同一个对象（指针相等），否则 deopt。常用于全局/内建对象的身份 guard。
- **`UseType`** `(val)`：无输出。当优化删除了对某寄存器的全部使用、但仍依赖其类型时插入，防止上游类型检查（如 GuardType）被死代码消除误删。
- **`HintType`** `(args...)`：携带 profiling 观察到的操作数类型组合（按频率排序）。仅提示性，不产生任何 LIR。
- **`Phi`** `(incoming...) → dst`：SSA 合并点，每个前驱基本块对应一个入参。由 SSAify 生成、PhiElimination/寄存器分配消除。
- **`GetSecondOutput`** `(src) → dst`：取上一条指令的第二个输出。用于返回多个 C 值的调用（第一个返回值走正常输出，第二个经此指令取出，如返回 int 的调用在 x64 上经 RDX 传回）。
- **`Cast`** `(receiver) → dst` ·deopt：Static Python 类型转换检查（pytype/optional/exact 参数），失败 deopt。
- **`CIntToCBool`** `(value) → dst`：CInt64 转 CBool（0/非0）。
- **`Snapshot`** `()`：不产生代码，仅携带一份 FrameState，为后续指令提供可用的 deopt 状态锚点。

### 5.2 函数序言与帧（6 条）

- **`LoadArg`** `(arg_idx) → dst`：读取当前函数第 N 个参数。只能出现在序言区（任何非 LoadArg 指令之前），类型可由静态参数注解指定。
- **`LoadCurrentFunc`** `() → dst`：加载当前 `PyFunctionObject*`。同样只能出现在序言区。
- **`LoadFrame`** `()`：从线程状态加载当前解释器帧指针。序言区专用，供 UpdatePrevInstr 等 使用。
- **`InitFrameCellVars`** `(func)`：（FT-Python）为帧初始化 cell 变量。
- **`UpdatePrevInstr`** `()`：维护 `_PyInterpreterFrame` 的 prev_instr/行号（3.11+ 特性），携带行号与所属内联层 parent。由 InsertUpdatePrevInstr pass 统一插入。
- **`OSREntry`** `(target_offset)` ·deopt：循环头部 OSR（栈上替换）二级入口的编译锚点，见 `docs/design/hot-loop-osr`。

### 5.3 对象级（动态类型）运算与比较（7 条）

- **`BinaryOp`** `(left, right) → dst` ·deopt：通用二元运算。`BinaryOpKind` 共 18 种：Add、And、FloorDivide、LShift、MatrixMultiply、Modulo、Multiply、Or、Power、RShift、Subscript、Subtract、TrueDivide、Xor 及 4 种无符号变体（FloorDivideUnsigned、ModuloUnsigned、RShiftUnsigned、PowerUnsigned）。来源：BINARY_OP / BINARY_SUBSCR 等。类型已知时被 Simplify 特化为 Long/Float/Int 系列或 UnicodeConcat 等。
- **`UnaryOp`** `(operand) → dst` ·deopt：通用一元运算（Not、Negate、Positive、Invert）。来源：UNARY_*。
- **`InPlaceOp`** `(left, right) → dst` ·deopt：就地运算（`+=` 等），`InPlaceOpKind` 13 种。来源：BINARY_OP 的 inplace 标志。
- **`Compare`** `(left, right) → dst` ·deopt：通用比较。`CompareOp` 16 种：6 种富比较 + In/NotIn + ExcMatch + 4 种无符号比较（`is`/`is not` 由 PrimitiveCompare 承载）。
- **`CompareBool`** `(left, right) → dst` ·deopt：同 Compare 但输出 CInt32，用于替代 `Compare + IsTruthy` 组合（常见于分支条件）。
- **`IsTruthy`** `(value) → dst` ·deopt：按 Python 真值语义输出 1/0。来源：POP_JUMP_IF_*、条件表达式等。
- **`IsInstance`** `(obj, type) → dst` ·deopt：isinstance() 检查。来源：opcode instanceof（Static Python）。

### 5.4 原始整型（primitive）特化（11 条）

这些指令操作未装箱的 C 整数（TCInt/TCBool），是"整型不装箱"优化的产物。

- **`IntBinaryOp`** `(left, right) → dst`：CInt/CBool 上的二元运算（下降为 LIR Add/Sub/Mul 等）。多由 FloatAccumulatorPromotion 之外的整型提升/拆箱链产生。
- **`PrimitiveUnaryOp`** `(value) → dst`：原始一元运算：NegateInt、InvertInt、NotInt。
- **`PrimitiveCompare`** `(left, right) → dst`：原始比较 10 种（含 4 种无符号）；Equal/NotEqual 也承载 `is`/`is not`（此时操作数可为任意类型）。
- **`PrimitiveConvert`** `(src) → dst`：原始类型间转换（目标类型由 type 参数给出，如 CInt32→CInt64）。
- **`PrimitiveBox`** `(value) → dst` ·deopt：把原始值装箱为 PyObject（如 CInt64→PyLong）。不支持 TCBool。
- **`PrimitiveBoxBool`** `(value) → dst`：CBool 装箱为 Py_True/Py_False（专设是因为 bool 单例语义）。
- **`PrimitiveUnbox`** `(value) → dst`：把 PyObject 拆箱为原始类型（type 参数）。与 PrimitiveUnboxCSE pass 配合去重。
- **`IndexUnbox`** `(value) → dst`：类似 PrimitiveUnbox 但采用 `PyNumber_AsSsize_t` 语义（溢出时抛 IndexError 而非 OverflowError），用于下标场景。
- **`IsCompactLong`** `(value) → dst`：检查 LongExact 是否为 compact（至多一个 30-bit digit）。
- **`CompactLongUnbox`** `(value) → dst`：把已验证 compact 的 Long 拆箱为 CInt64（不再次检查）。
- **`LoadVarObjectSize`** `(obj) → dst`：把 `PyVarObject` 的 ob_size 读为 CInt64（list/tuple/str 等的长度快速路径）。

### 5.5 浮点特化（3 条）

- **`DoubleBinaryOp`** `(left, right) → dst`：CDouble 上的二元运算（FloatAccumulatorPromotion 的产物，纯机器运算）。
- **`FloatBinaryOp`** `(left, right) → dst` ·deopt：FloatExact 对象上的二元运算，经 PyFloat 的 nb_* slot method 调用。
- **`FloatCompare`** `(left, right) → dst`：FloatExact 比较（ARM 上由 FloatComparisonSimplification 进一步改写）。

### 5.6 PyLong / Unicode 特化（8 条）

- **`LongBinaryOp`** `(left, right) → dst` ·deopt：LongExact 二元运算，经 PyLong nb_* slot。
- **`LongInPlaceOp`** `(left, right) → dst` ·deopt：LongExact 就地运算。
- **`LongCompare`** `(left, right) → dst`：LongExact 比较。
- **`UnicodeCompare`** `(left, right) → dst`：str 比较。
- **`UnicodeConcat`** `(left, right) → dst` ·deopt：str 拼接。
- **`UnicodeRepeat`** `(str, count) → dst` ·deopt：str 重复（`s * n`）。
- **`UnicodeSubscr`** `(str, index) → dst` ·deopt：str 下标取字符。
- **`CopyDictWithoutKeys`** `(dict, keys) → dst` ·deopt：构造去掉若干键的 dict 副本（dict 展示协议 `__repr__` 相关及 pattern match 场景）。

### 5.7 调用类（10 条）

- **`VectorCall`** `(func, args...) → dst` ·deopt：**最常用的调用指令**。操作数 0 为被调对象，其余为实参；flags 支持 KwArgs（kwargs 以 `name=value` 形式附加）。来源：CALL/CALL_KW 等字节码。被调方已被 JIT 编译且签名匹配时会被改写为 `InvokeStaticFunction`。
- **`CallEx`** `(func, pargs, kwargs) → dst` ·deopt：实参已打包为 tuple/dict 的调用（CALL_FUNCTION_EX）。
- **`CallMethod`** `(func, self, args...) → dst` ·deopt：与 LoadMethod 配对的方法调用；操作数 1 是方法查找时的 receiver。
- **`CallInd`** `(funcptr, args...) → dst` ·deopt：经寄存器中的 C 函数指针间接调用。返回值判错约定：PyObject 返回 NULL 即错误；返回原始类型时以 RDX（int）或 XMM1（float）是否为零判错。
- **`CallStatic`** `(args...) → dst`：调用编译期已知地址的 C 函数（addr + 返回类型）。主要供内部/runtime helper 调用。
- **`CallStaticRetVoid`** `(args...)`：同上，无返回值。
- **`CallCFunc`** `(args...) → dst`：调用白名单中的固定 C 函数（`CallCFunc_FUNCS`：Cix_PyAsyncGenValueWrapperNew、JitCoro_GetAwaitableIter、JitGen_yf），为 HIR 可序列化预留。
- **`CallIntrinsic`** `(args...) → dst`：按索引调用 intrinsic 表中的函数（操作 runtime 内部入口）。
- **`InvokeStaticFunction`** `(args...) → dst` ·deopt：Static Python 直接调用——被调函数有静态入口点，按 x64 调用约定直接传参。
- **`LoadFunctionIndirect`** `() → dst` ·deopt：经函数指针槽位加载被调对象（Static Python 间接调用前半部分），带描述符。

### 5.8 属性（attribute）访问（21 条）

- **`LoadAttr`** `(receiver) → dst` ·deopt：通用属性加载，name 由 `co_names` 索引指定。来源：LOAD_ATTR（非方法形态）。
- **`LoadAttrCached`** `(receiver) → dst` ·deopt：带 inline cache 的属性加载（LOAD_ATTR 的_icache 版本）。
- **`LoadAttrSpecial`** `(receiver) → dst` ·deopt：跳过实例 dict、但保留描述符协议的 special 属性加载（Static Python），失败时按 failure_fmt_str 报错。
- **`LoadAttrSuper`** `(global_super, type, receiver) → dst` ·deopt：`super().attr` 形式的属性加载；receiver 决定 MRO 搜索起点。
- **`StoreAttr`** `(receiver, value)` ·deopt：属性存储。来源：STORE_ATTR。
- **`StoreAttrCached`** `(receiver, value)` ·deopt：带 inline cache 的存储。
- **`DeleteAttr`** `(receiver)` ·deopt：属性删除（DELETE_ATTR）。
- **`LoadField`** `(receiver) → dst`：按编译期 offset 直接加载对象字段（KnownClass/静态字段），可标记借用。
- **`StoreField`** `(receiver, value, previous)`：按 offset 直接存储字段；第三个操作数保住旧值的存活以配合引用计数插入。
- **`LoadFieldAddress`** `(object, offset) → dst`：计算 `object + offset` 的地址（返回 CPtr），供间接访问。
- **`LoadMethod`** `(receiver) → dst` ·deopt：方法加载（LOAD_METHOD），输出 method_and_self 结构，配对 CallMethod。
- **`LoadMethodCached`** `(receiver) → dst` ·deopt：inline cache 版方法加载。
- **`LoadModuleAttrCached`** `(receiver) → dst` ·deopt：从模块对象加载属性的缓存特化。
- **`LoadModuleMethodCached`** `(receiver) → dst` ·deopt：从模块加载方法的缓存特化。
- **`LoadMethodSuper`** `(global_super, type, receiver) → dst` ·deopt：`super().method` 加载。
- 类型缓存四件套（把对 type 对象的属性/方法查找拆为"填充缓存 + 读缓存"两段，使热路径只剩读取与 guard）：
  - **`FillTypeAttrCache`** `(receiver) → dst` ·deopt：执行完整属性查找并填充 cache_id 指定的缓存；
  - **`LoadTypeAttrCacheEntryType`** `(cache_id) → dst`：读缓存条目的类型（用于 guard）；
  - **`LoadTypeAttrCacheEntryValue`** `(cache_id) → dst`：读缓存条目的值；
  - **`FillTypeMethodCache`** / **`LoadTypeMethodCacheEntryType`** / **`LoadTypeMethodCacheEntryValue`**：方法版本的对应三件套。

### 5.9 变量、作用域与 cell（11 条）

- **`LoadGlobal`** `() → dst` ·deopt：按 `co_names` 索引加载全局变量（LOAD_GLOBAL）。
- **`LoadGlobalCached`** `() → dst`：绑定 code/builtins/globals 的全局加载缓存版本。
- **`CheckVar`** `(value) → dst` ·deopt：value 为 NULL 时抛 `UnboundLocalError`（名字内嵌）。LOAD_FAST 的判空显式化。
- **`CheckFreevar`** `(value) → dst` ·deopt：同上，抛 `NameError`（自由变量场景，LOAD_DEREF 判空）。
- **`CheckField`** `(value) → dst` ·deopt：同上，抛 `AttributeError`（字段场景）。
- **`LoadCellItem`** `(cell) → dst`：读 cell 内容（借引用）。
- **`StealCellItem`** `(cell) → dst`：读 cell 并窃取其引用，仅用作 SetCellItem 的前置。
- **`SwapCellItem`** `(cell, new_value) → dst`：原子交换 cell 内容并返回旧值（FT-Python 线程安全 STORE_DEREF）。
- **`SetCellItem`** `(cell, src, old)`：写 cell；old 操作数保住旧值存活到写入之后。
- **`MakeCell`** `(value) → dst` ·deopt：新建持有 value 的 cell（`PyCell_New`，隐式 incref）。
- **`SetFunctionAttr`** `(value, base, field)`：写函数对象的 closure/annotations/kwdefaults/defaults/annotate（3.14+）字段。MAKE_FUNCTION 的配套。

### 5.10 容器构造（17 条）

- **`MakeList`** `(elements...) → dst` ·deopt：构造 list（BUILD_LIST）。
- **`MakeTuple`** `(elements...) → dst` ·deopt：构造 tuple（BUILD_TUPLE / LOAD_CONST tuple 展开）。
- **`MakeTupleFromList`** `(list) → dst` ·deopt：由 list 构造 tuple。
- **`MakeDict`** `(capacity) → dst` ·deopt：构造空 dict（BUILD_MAP），capacity=0 用默认值。
- **`MakeCheckedDict`** `(capacity, dict_type) → dst` ·deopt：构造 Static Python CheckedDict。
- **`MakeCheckedList`** `(elements...) → dst` ·deopt：构造 Static Python CheckedList。
- **`MakeSet`** `() → dst` ·deopt：构造空 set（BUILD_SET）。
- **`MakeFunction`** `(code, qualname) → dst` ·deopt：构造函数对象（MAKE_FUNCTION）。
- **`TpAlloc`** `() → dst` ·deopt：按 `tp_alloc` 分配指定类型的实例（Static Python `__new__` 快速路径）。
- **`GetTuple`** `(seq) → dst` ·deopt：把任意序列转为 tuple。
- **`LoadTupleItem`** `(tuple, idx) → dst`：按编译期下标读 tuple 元素，**无边界检查**（下标由编译期常量保证合法）。
- **`LoadArrayItem`** `(ob_item, idx, seq) → dst`：等价 `((type*)((char*)ob_item + offset))[idx]`；seq 操作数仅为保住容器存活（引用计数插入用）。
- **`StoreArrayItem`** `(ob_item, idx, value, container)`：对应的数组元素存储。
- **`LoadSplitDictItem`** `(dict, item_idx) → dst`：读 split-table dict 的 `ma_values[item_idx]`（item_idx 来自 `_PyDictKeys_GetSplitIndex`）。
- **`CheckSequenceBounds`** `(array, idx) → dst` ·deopt：检查下标越界并归一化负数下标；越界返回 -1。
- **`BuildString`** `(parts...) → dst` ·deopt：拼接构造 str（BUILD_STRING）。
- **`BuildSlice`** `(start, stop[, step]) → dst` ·deopt：构造 slice 对象（BUILD_SLICE，2 或 3 操作数）。

### 5.11 容器操作与下标（12 条）

- **`ListAppend`** `(list, item) → dst` ·deopt：list 追加（LIST_APPEND）。
- **`ListExtend`** `(list, iterable) → dst` ·deopt：list 扩展（LIST_EXTEND）。
- **`SetDictItem`** `(dict, key, value) → dst` ·deopt：dict 写入（MAP_ADD）。
- **`SetSetItem`** `(set, key) → dst` ·deopt：set 写入（SET_ADD）。
- **`SetUpdate`** `(set, iterable) → dst` ·deopt：set 扩展（SET_UPDATE）。
- **`DictUpdate`** `(dict, obj) → dst` ·deopt：经 `PyDict_Update` 合并（DICT_MERGE/UPDATE 之一）。
- **`DictMerge`** `(dict, obj, func) → dst` ·deopt：经 `_PyDict_MergeEx` 合并（带 override 参数场景）。
- **`DictSubscr`** `(dict, key) → dst` ·deopt：精确 dict 的 `d[k]`。
- **`StoreSubscr`** `(container, sub, value)` ·deopt：通用下标存储 `c[s] = v`（STORE_SUBSCR）；出错时往 dst 放 NULL。
- **`DeleteSubscr`** `(container, sub)` ·deopt：`del c[s]`（DELETE_SUBSCR）。
- **`MergeSetUnpack`** `(set, iterable) → dst` ·deopt：经 `_PySet_Update` 合并（SETUP_FINALLY 的 unpack 场景）。
- **`GetLength`** `(obj) → dst` ·deopt：调用 `__len__` 取长度（len() 与 for 循环计数）。

### 5.12 迭代器与生成器（12 条）

- **`GetIter`** `(iterable) → dst` ·deopt：取迭代器；若本身就是迭代器则原样返回（GET_ITER）。
- **`InvokeIterNext`** `(iter) → dst` ·deopt：调用 `next(iter)`。输出三态：迭代器耗尽的**哨兵值** / NULL（出错）/ 正常值。
- **`CondBranchIterNotDone`** `(value)`：按 InvokeIterNext 的输出分支——非哨兵走 true_bb，哨兵走 false_bb。FOR_ITER 的循环条件。
- **`GetAIter`** `(obj) → dst` ·deopt：异步迭代器（GET_AITER）。
- **`GetANext`** `(aiter) → dst` ·deopt：取下一个 awaitable（GET_ANEXT）。
- **`Send`** `(iter, value) → dst` ·deopt：`iter.send(value)`，用于 yield from / async for；handleStopAsyncIteration 控制是否把 StopAsyncIteration 视为正常结束。
- **`YieldValue`** `(value) → dst` ·deopt：生成器 yield；可附加 yieldFromIter 表示 yield-from 形态。输出为 send 回来的值。
- **`InitialYield`** `() → dst` ·deopt：生成器首次挂起点，返回持有状态的生成器对象；每个生成器函数恰好一条（RETURN_GENERATOR）。
- **`SetCurrentAwaiter`** `(awaiter)`：设置 per-frame awaiter（异步优化路径）。
- **`WaitHandleLoadCoroOrResult`** `(wait_handle) → dst`：从 wait handle 取协程或结果（async 并行调度，Static Python）。
- **`WaitHandleLoadWaiter`** `(wait_handle) → dst`：取 wait handle 的 waiter；**`WaitHandleRelease`** `(wait_handle)`：释放 wait handle。

### 5.13 控制流与去优化（8 条）

- **`Branch`** `()`：无条件跳转到 target 块（终结指令）。
- **`CondBranch`** `(cond)`：cond 非零跳 true_bb，否则 false_bb（终结指令）。`CondBranch<true_bb, false_bb>`。
- **`CondBranchCheckType`** `(target)`：操作数类型匹配 type 时走 true_bb，否则 false_bb。类型细化路径的专用分支。
- **`Return`** `(val)`：函数返回（终结指令），携带返回值类型。
- **`Unreachable`** `()`：静态不可达路径的标记；运行时执行到它即是编译器 bug。
- **`Guard`** `(value)` ·deopt：验证 value 非零，失败即 deopt。最通用的运行时假设检查，与 `GuardType`/`GuardIs` 一起构成去优化 guard 家族。
- **`Deopt`** `()` ·deopt：无条件 deopt——总是回到解释器。用于编译器无法继续生成代码的路径。
- **`DeoptPatchpoint`** `()` ·deopt：在指令流中预留一段空间，运行时可被改写为 Deopt 跳转（配合 `Jit/deopt_patcher.h`）。供"编译代码所依赖的不变量在运行时失效则主动打补丁失效代码"的优化使用。

### 5.14 检查与异常（7 条）

- **`CheckErrOccurred`** `()` ·deopt：检查 `_PyErr_Occurred()`，有异常则转入异常处理路径。
- **`CheckExc`** `(var) → dst` ·deopt：var 为 NULL 表示有异常，转异常处理；否则透传输出。
- **`CheckNeg`** `(code) → dst` ·deopt：返回码为负表示错误，转异常处理；否则透传。
- **`IsNegativeAndErrOccurred`** `(code) → dst` ·deopt：code 为负**且**设置了异常才转异常处理（用于区分"负值是合法返回"的场景）。
- **`Raise`** `()` ·deopt：重新抛出当前异常（RERAISE）。
- **`RaiseStatic`** `() → dst` ·deopt：`PyErr_Format(exc_type, fmt)` 后抛出，用于运行时断言类错误。
- **`RaiseAwaitableError`** `(type)` ·deopt：'async with' 取迭代器失败时格式化抛错（is_aenter 区分 `__aenter__`/`__aexit__`）。

### 5.15 f-string、t-string 与格式化（6 条）

- **`FormatValue`** `(fmt_spec, value) → dst` ·deopt：FORMAT_VALUE，conversion 指定 `!s/!r/!a`。
- **`FormatWithSpec`** `(value, spec) → dst` ·deopt：FORMAT_WITH_SPEC（带 format spec 的格式化）。
- **`ConvertValue`** `(value) → dst` ·deopt：按 converter_idx 做 `!s/!r/!a` 转换。
- **`BuildInterpolation`** `(value, str, format) → dst` ·deopt：构造插值对象（t-string BUILD_INTERPOLATION，conversion 属性）。
- **`BuildTemplate`** `(strings, interpolations) → dst` ·deopt：构造 t-string 模板（BUILD_TEMPLATE）。
- **`LoadSpecial`** `(self) → dst` ·deopt：按 special_idx 加载特殊方法（如 `__aenter__`/`__aexit__` 的 LOAD_SPECIAL）。

### 5.16 import / match / 解包（8 条）

- **`ImportName`** `(fromlist, level) → dst` ·deopt：IMPORT_NAME。
- **`EagerImportName`** `(fromlist, level) → dst` ·deopt：eager 模式的 import（编译期未解析时）。
- **`ImportFrom`** `(module) → dst` ·deopt：IMPORT_FROM。
- **`MatchClass`** `(subject, type, nargs, kwargs) → dst`：match 语句 class pattern 的核心步骤（调 match_class()）。
- **`MatchKeys`** `(subject, keys) → dst` ·deopt：match 语句 mapping pattern 的取键（match_keys()：错误返回 NULL、不匹配返回 None、匹配返回值 tuple）。
- **`UnpackExToTuple`** `(seq) → dst` ·deopt：UNPACK_EX 解包为 tuple（before/after 指定 `a, *b, c` 两侧数量）。
- **`UnpackSequence`** `(seq, items_ptr) → dst`：经迭代协议解包恰好 count 项到 items_ptr 指向的数组（UNPACK_SEQUENCE 慢路径）。配套的 **`ReserveStack`** `(num_words) → dst`：在调用参数区下方预留指针大小临时数组的栈空间，输出指向该空间的 CPtr。

### 5.17 引用计数（5 条，RefcountInsertion pass 生成）

引用计数的插入策略详见 `Jit/hir/refcount_insertion.md`。

- **`Incref`** `(reg)`：引用计数 +1。
- **`Decref`** `(reg)`：引用计数 -1（可能释放对象）。
- **`XIncref`** `(opt_reg)`：可为 NULL 的 Incref。
- **`XDecref`** `(opt_reg)`：可为 NULL 的 Decref。
- **`BatchDecref`** `(regs...)`：批量递减（连续多个 Decref 的合并形态）。

### 5.18 运行时协作（3 条）

- **`LoadEvalBreaker`** `() → dst`：读取 eval breaker；非零表示需要释放 GIL / 跑 signal handler / 周期任务。约定后面紧跟 CondBranch。来源：循环回边、调用点等安全检查位置。
- **`RunPeriodicTasks`** `() → dst` ·deopt：执行 `_PyRunPeriodicTasks`（让出 GIL、处理信号等）。
- **`AtQuiescentState`** `()`：free-threading QSBR 的静默状态声明（无操作锚点）。

### 5.19 内联边界（2 条）

- **`BeginInlinedFunction`** `()`：内联展开的起始标记。持有被内联函数的 `PyFunctionObject`、调用者的 FrameState（构成 FrameState 父链，供嵌套 deopt 回放）与 reifier。
- **`EndInlinedFunction`** `()`：内联结束标记，指回配对的 Begin。

### 5.20 TreeIter 状态机（13 条，TreeIterStateMachinePass 生成）

由 TreeIterStateMachinePass 生成，把递归 yield-from 树遍历改写为显式中序状态机（详见 `docs/design/generators-jit-treeiter-state-machine`）。状态保存在 JIT 帧 footer 的 `tree_iter_state` 堆结构中。

- **`EnsureTreeIterState`** `() → dst` ·deopt：确保 tree_iter_state 已分配，失败抛 MemoryError。
- **`SaveCurrentNode`** `(node)`：写 current_node（新值 incref、旧值 decref）。
- **`LoadCurrentNode`** `() → dst`：读 current_node（带 incref）。
- **`SavePhase`** `(phase)` / **`LoadPhase`** `() → dst`：写/读 current_phase（CInt32）。
- **`StateStackPush`** `(node, phase) → dst` ·deopt：压入 (node, phase)，扩容失败抛 MemoryError。
- **`StateStackPop`** `() → dst`：弹出栈顶，node 所有权转移给输出，phase 写入 popped_phase。
- **`LoadPoppedPhase`** `() → dst`：读 StateStackPop 写入的 popped_phase。
- **`LoadStackTop`** `() → dst`：读栈顶（不弹出）。
- **`CheckTreeIterChildEntry`** `(child) → dst` ·deopt：生产门控——验证 child 的 exact 类型、active-path 一致性与深度预算。
- **`TreeIterEnterChild`** `(child)`：记录进入 child（active-path + 深度 +1）。
- **`TreeIterLeaveCurrentNode`** `()`：离开当前节点（active-path - 深度 -1）。
- **`ClearTreeIterState`** `()`：释放状态中全部持有引用并置空 footer 指针。

## 6. HIR → LIR 下降要点

HIR 到 LIR 的下降在 `Jit/lir/generator.cpp` 的 `LIRGenerator::TranslateOneBasicBlock` 中按 HIR opcode 逐条 switch 完成，两条代表性路径：

- `BinaryOp<Add>` → 查 `binaryfunc` 助手表（`PyNumber_Add` 等）→ LIR `Call`（返回值后跟 LIR `Guard` 判 NULL）；
- `VectorCall` → 优先尝试特化调用（`TranslateSpecializedCall`），否则生成 LIR `VectorCall`（函数指针与 flags 作为立即数输入，实参逐个以 VReg 附着）；
- 可 deopt 指令 → `appendGuard` 生成 LIR `Guard`（输入为 guard kind、deopt id、被查值、目标对象/类型、live 值列表）。

## 7. 延伸阅读

- `cinderx/Jit/guide.md`：JIT 总体开发指南（含端到端示例）；
- `cinderx/Jit/hir/type.md`：HIR 类型系统；
- `cinderx/Jit/hir/refcount_insertion.md`：引用计数插入 pass；
- `cinderx/Jit/deoptimization.md`：deopt / FrameState 机制；
- 本仓库 `docs/guide/LIR指令指南.md`：LIR 指令全集。
