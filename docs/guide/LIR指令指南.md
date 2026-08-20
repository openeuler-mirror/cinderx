# CinderX LIR 指令完整指南

> 适用范围：本仓库 `cinderx/Jit/lir/` 下的 LIR（Low-level Intermediate Representation，低层中间表示）。
> 指令全集以 `cinderx/Jit/lir/instruction.h` 中的 `FOREACH_INSTR_TYPE` 宏为准，当前共 **113 条**；指令语义辅助函数（isCompare、isTerminator 等）位于 `cinderx/Jit/lir/instruction.cpp`。

## 1. LIR 是什么

LIR 是 CinderX JIT 中介于 HIR 与机器码之间的"对汇编的一层薄抽象"。它在 SSA 形式下描述操作，刻意贴近 x86-64 指令集的形态（同时也支持 AArch64 后端），每条 LIR 指令最终由 asmjit 翻译为一条或数条机器指令。

设计要点（见 `instruction.h:20-83` 的注释）：

- 每条指令**至多一个输出**、任意多个输入；逻辑上无输出的指令其输出 operand 类型为 `kNone`；
- 每条指令带有六个**属性**（见 3.2），用于寄存器分配、死代码消除与 codegen 约束；
- 指令通过 `origin()` 指回它由哪条 HIR 指令下降而来，dump 机器码时据此穿插 HIR 注释。

## 2. LIR 在编译管线中的位置

```
最终 HIR
   ▼  LIRGenerator::TranslateOneBasicBlock（Jit/lir/generator.cpp，按 HIR opcode switch 逐条下降）
LIR（SSA + 虚拟寄存器）
   ▼  PostGenerationRewrite（Jit/lir/postgen.cpp，规范化：常量折叠、大立即数、ARM 子字长等）
   ▼  eliminateDeadCode（Jit/lir/dce.cpp，标记-清除 DCE）
   ▼  LinearScanAllocator（Jit/lir/regalloc.cpp，线性扫描寄存器分配，同时计算 spill 栈帧）
   ▼  PostRegAllocRewrite（Jit/lir/postalloc.cpp，operand 物理化、消除 Phi、move 序列优化、x86 除法改写）
   ▼  verify（Jit/lir/verify.cpp，不变式校验）
   ▼  AutoTranslator::translateInstr（Jit/codegen/autogen.cpp，#if 分架构的 per-opcode 发射）
机器码（经 asmjit：x86::Builder / a64::Builder，见 Jit/codegen/arch.h）
```

上述流程由 `Jit/codegen/gen_asm.cpp` 的 `NativeGenerator::GetEntryPoint()` 驱动；LIR 会在 generation / postgen / regalloc / postalloc 四个时点各 dump 一次。

## 3. 核心概念

### 3.1 Instruction 类

`jit::lir::Instruction`（`instruction.h:219`）持有：

- `id()`：函数内唯一编号（dump 中 `%id` 即虚拟寄存器名）；
- `opcode()`：113 种之一，`opname()` 返回字符串名；
- `output()`：唯一输出 operand（可为 kNone）；
- `inputs_`：输入 operand 列表，数量可变；
- `origin()`：来源 HIR 指令；
- `basicblock()`：所属 LIR 基本块。

### 3.2 指令六属性（FOREACH_INSTR_TYPE 的可选实参）

`X(name, inputs_live_across, flag_effects, opnd_size_type, out_phy_use, in_phy_uses, is_essential)`

| 属性 | 默认值 | 含义 |
|---|---|---|
| `inputs_live_across` | false | 输入是否活跃到指令**结束**。false 时输出可与某输入共用寄存器（两地址式）；true 时 codegen 可在写输出后再读输入，代价是略高的寄存器压力（典型：不可交换的 Sub/Fsub/Fdiv、Select） |
| `flag_effects` | kNone | 对机器状态标志的影响：`kNone` 不修改；`kSet` 设置为有意义值（比较/算术）；`kInvalidate` 破坏标志（调用类） |
| `opnd_size_type` | kDefault | 操作数宽度：`kDefault` 按 DataType；`kAlways64` 全 64 位；`kOut` 与输出同宽（无输出时与第一个输入同宽） |
| `out_phy_use` | true | 输出是否必须分配物理寄存器（false 可给栈槽） |
| `in_phy_uses` | {} | vector<bool>，为 1 的下标对应输入必须落在物理寄存器 |
| `is_essential` | false | 有副作用、绝不能被 DCE 删除。无输出的指令原则上必须 essential |

### 3.3 Operand 与 DataType

Operand 包装类由 `Jit/lir/operand.h` 生成，作为 `addOperands(...)` 的实参使用（`Out*` 前缀为输出变体，输出必须排在第一位）：

| Operand | 含义 | dump 形态 |
|---|---|---|
| `PhyReg(loc, ty)` / `OutPhyReg` | 物理寄存器 | `RDI:Object` |
| `Stk(slot, ty)` / `OutStk` | 栈槽 | `stack[n]` |
| `VReg(def_instr)` | 虚拟寄存器，指向定义它的 LIR 指令 | `%42:Object` |
| `Imm(v, ty)` / `FPImm(d)` | 整数/双精度立即数 | `123(0x7b):64bit` |
| `MemImm(addr)` | 固定内存地址（函数指针、常量对象） | `17124416(0x1054c40):Object` |
| `Lbl(block)` / `AsmLbl(label)` | 基本块标签 / asmjit 标签 | `BB%3` |
| `Ind(base, index, mult, offset)` | 间接寻址 base+index*scale+offset | `[R14 + RAX*8 + 0x10]` |

`DataType`（`Jit/lir/type.h`）：`k8bit / k16bit / k32bit / k64bit / kDouble / kObject`——四种宽度整数、双精度浮点、PyObject 指针。

### 3.4 Guard 的条件种类

`InstrGuardKind`（`instruction.h:468`）：`kAlwaysFail`（必然 deopt）、`kHasType`（类型 guard）、`kIs`（对象身份 guard）、`kNotNegative`、`kNotZero`、`kZero`。

### 3.5 指令分类函数（instruction.cpp）

| 函数 | 覆盖的 opcode |
|---|---|
| `isCompare()` | Equal、NotEqual、有/无符号 × </<=/>/>= 共 10 个 |
| `isBranchCC()` | 18 个 BranchCC（Z/NZ/E/NE/A/AE/B/BE/G/GE/L/LE/C/NC/O/NO/S/NS） |
| `isAnyBranch()` | CondBranch ∪ isBranchCC() |
| `isTerminator()` | Return、BranchToYieldExit、EpilogueEnd |
| `isAnyYield()` | YieldInitial、StoreGenYieldPoint、StoreGenYieldFromPoint |
| `isCallLike()` | Call、LoadAttrCachedFastPath、VarArgCall、VectorCall |

另有静态工具：`negateBranchCC()`（取反分支条件）、`flipBranchCCDirection()`（交换比较方向）、`flipComparisonDirection()`、`compareToBranchCC()`（比较 opcode → 等价条件分支 opcode）。

## 4. 如何 dump 与阅读 LIR

### 4.1 环境变量

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITDUMPLIR` | 在 generation / postgen / regalloc / postalloc 四个时点 dump LIR |
| `PYTHONJITDUMPLIRORIGIN` | LIR dump 中附带来源 HIR 指令注释 |
| `PYTHONJITDUMPASM` | dump 最终汇编（含 HIR 注释） |
| `PYTHONJITASMSYNTAX=intel\|att` | 汇编语法（默认随平台） |
| `PYTHONJITLOGFILE=path` | 输出重定向到文件 |
| `PYTHONJITLIRINLINER` | LIR 级调用内联开关 |
| `PYTHONJITDEBUGREGALLOC` | 寄存器分配调试输出 |

示例：

```bash
python -X jit-all -X jit-dump-lir -X jit-dump-lir-origin demo.py
```

### 4.2 输出格式

打印器为 `Jit/lir/printer.cpp`，格式：`输出operand = InstrName 输入1, 输入2`；块头 `BB %1 - preds: %0 - succs: %2`；HIR origin 以 `# ` 前缀穿插。示例（摘自 `Jit/guide.md`，HIR `BinaryOp<Add>` 下降结果）：

```
# v13:Object = BinaryOp<Add> v6 v7 { ... }
      RDI:Object = Move R12:Object
      RSI:Object = Move R13:Object
      RAX:Object = Move 17124416(0x1054c40):Object
                   Call RAX:Object
                   Guard 0(0x0):64bit, 1(0x1):Object, RAX:Object, 0(0x0):Object, R12:Object, R14:Object, R13:Object
```

解读：先把两个实参 move 到 x64 SysV 的 RDI/RSI，把 helper 函数指针装入 RAX，`Call` 后用 `Guard`（输入依次为 guard kind=0、deopt id=1、被查值 RAX、目标 Imm 0、live 值 R12/R14/R13）确保返回值非 NULL，否则 deopt。

## 5. LIR pass 说明

| Pass（文件） | 阶段 | 作用 |
|---|---|---|
| `PostGenerationRewrite`（postgen.cpp） | regalloc 前 | 规范化：一元运算常量折叠、大立即数降级、x86 内存大常量、ARM 子字长/立即数/调用输入重写；内嵌触发 LIR 内联（inliner.cpp 把已编译 callee 的 LIR 拼入 caller） |
| `eliminateDeadCode`（dce.cpp） | regalloc 前 | 标记-清除 DCE，保留分支/副作用/内存写（isUseful 判据） |
| `LinearScanAllocator`（regalloc.cpp） | 中枢 | SSA 线性扫描寄存器分配（Wimmer 算法），同时确定 spill 栈帧大小 |
| `PostRegAllocRewrite`（postalloc.cpp) | regalloc 后 | operand 物理化、消除 Phi（改写为 move 序列并优化）、x86 除法序列改写等 |
| `blocksorter.cpp` | 布局 | 基本块排序 |
| `verify.cpp` | 校验 | postalloc 后不变式检查 |
| `rewrite.cpp/h` | 框架 | 按 Function/BB/Instr 粒度注册回调的重写框架，postgen/postalloc 均派生自它 |
| `parser.cpp` | 工具 | 文本 LIR 解析（供测试） |

## 6. 全量指令参考（113 条）

### 6.1 伪指令与元指令（3 条）

- **`Bind`**：不产生机器码。把物理寄存器与预定义值（如按调用约定进入 JIT 函数的参数）关联到虚拟寄存器，供寄存器分配器建立约束。
- **`Nop`**：占位空指令。
- **`Unreachable`**：不可达路径标记，essential（不被 DCE 删除）。

### 6.2 调用类（7 条）

- **`Call`**：通用 C 调用（x64 SysV / AArch64 AAPCS64）。函数指针可来自寄存器或 `MemImm`。返回值判错由后续 `Guard` 完成。
- **`VectorCall`**：`_PyObject_Vectorcall` 专用形态：输入 0 为函数对象（必须物理寄存器），输入 1 为 flags 立即数，其余为实参。HIR VectorCall 的主要下降目标。
- **`VarArgCall`**：变长参数调用形态（配合 `VariadicPush`/`Cqo` 等）。
- **`LoadAttrCachedFastPath`**：inline-cache 属性加载的快路径整体（call-like，含缓存探测）。
- **`LoadArg`**：从进入约定位置读取第 N 个参数。HIR `LoadArg` 的下降产物。
- **`LoadSecondCallResult`**：读取调用的第二返回值（x64 为 RDX；配合 HIR `GetSecondOutput`）。
- **`LoadThreadState`**：加载当前 `PyThreadState*`（eval breaker、帧访问等使用）。

### 6.3 Guard / deopt / OSR（3 条）

- **`Guard`**：运行时守卫，失败跳转 deopt。输入布局（由 `generator.h` 的 `appendGuard` 生成）：[0] guard kind（Imm，取值见 3.4）、[1] deopt metadata id（Imm）、[2] 被检查值、[3] `GuardIs` 的目标对象 / `GuardType` 的类型对象（MemImm）或 Imm 0、[4..] deopt 所需 live 值。输入 2、3 必须物理寄存器。
- **`DeoptPatchpoint`**：在指令流中预留可在运行时被改写成 deopt 跳转的补丁点（配合 `Jit/deopt_patcher.h`，用于依赖失效时打补丁）。输入 0、1 必须物理寄存器。
- **`OSREntry`**：OSR 二级入口锚点（HIR OSREntry 下降产物）。

### 6.4 数据移动（7 条）

- **`Move`** / **`MoveRelaxed`**：寄存器/栈/立即数间搬运；Relaxed 放宽目的地约束（输出可为栈槽等）。输出宽度跟随操作数（kOut）。若输出是内存间接地址，输入强制物理寄存器（避免 mem→mem move，见 `instruction.cpp:218`）。
- **`MovConstPool`**：从常量池加载常量/地址。
- **`Exchange`**：交换两个操作数（xchg 语义）。
- **`Push`** / **`Pop`**：栈压入/弹出，essential。
- **`VariadicPush`**：变长参数序列压栈。

### 6.5 整数算术与移位（18 条）

- **`Add`** / **`Sub`** / **`Mul`**：加减乘，置标志。Sub 输入跨指令存活（codegen 需在写输出后仍能读操作数）。
- **`Div`** / **`DivUn`**：有符号/无符号除法；除数（输入 0）必须物理寄存器；x64 上配合 `Cdq/Cqo` 与 postalloc 的序列改写。
- **`And`** / **`Or`** / **`Xor`**：位运算，置标志。
- **`Negate`**（取负，置标志）、**`Invert`**（按位取反）、**`Inc`** / **`Dec`**（自增/自减，置标志）。
- **`MulAdd`**：三操作数乘加 `a*b + c`，64 位。
- **`LShift`** / **`RShift`** / **`RShiftUn`**：左移 / 算术右移 / 逻辑（无符号）右移，置标志。
- **`Lea`**：地址计算 `base + index*scale + offset`，64 位。
- **`ReserveStack`**：在调用参数区下方预留栈空间（HIR ReserveStack 下降产物），输出指向保留区。

### 6.6 扩展与标量转换（10 条）

- **`Sext`** / **`Zext`**：符号/零扩展。
- **`MovZX`** / **`MovSX`** / **`MovSXD`**：x86 风格"移动并扩展"（零扩展 / 符号扩展 / 符号扩展至 64 位）。
- **`Int64ToDouble`**：int64 → double（x64 `cvtsi2sd`）。
- **`IntToBool`**：整数归约为 0/1 并置标志。
- **`Cdq`** / **`Cwd`** / **`Cqo`**：除法辅助——把累加器符号扩展到 DX/EDX/RDX（16/32/64 位），essential。

### 6.7 浮点算术（4 条）

- **`Fadd`** / **`Fsub`** / **`Fmul`** / **`Fdiv`**：SSE 双精度算术，操作数 64 位。Fsub/Fdiv 输入跨指令存活（不可交换运算，codegen 写输出后仍需读源操作数）。

### 6.8 比较与测试（14 条）

- **`Cmp`**：x86 `cmp` 风格——只设置标志，配合后续 BranchCC 使用（输出宽度 kOut）。
- **`Test`** / **`Test32`**：按 AND 语义置标志（32 位变体），无值输出。
- **`Equal`** / **`NotEqual`** / **`GreaterThanSigned`** / **`LessThanSigned`** / **`GreaterThanEqualSigned`** / **`LessThanEqualSigned`** / **`GreaterThanUnsigned`** / **`LessThanUnsigned`** / **`GreaterThanEqualUnsigned`** / **`LessThanEqualUnsigned`**：产生 0/1 结果的比较（下降为 `cmp` + `setcc`）。
- **`BitTest`**：位测试（x86 `bt`），置标志。

### 6.9 分支（21 条）

- **`Branch`**：无条件跳转（目标为 Lbl 输入）。
- **`CondBranch`**：通用条件分支：输入 0（物理寄存器）非零则跳转；flag 为 kInvalidate。多由通用条件逻辑使用，具体比较场景优先用 Cmp+BranchCC。
- 18 个 **`BranchCC`**（标志条件跳转，配套 `Cmp`/`Test`/算术指令产生的标志）：

| 后缀 | 条件 | 后缀 | 条件 |
|---|---|---|---|
| `Z` / `NZ` | 为零 / 非零 | `E` / `NE` | 相等 / 不等 |
| `A` / `AE` | 无符号 > / >= | `B` / `BE` | 无符号 < / <= |
| `G` / `GE` | 有符号 > / >= | `L` / `LE` | 有符号 < / <= |
| `C` / `NC` | 有进位 / 无进位 | `O` / `NO` | 有溢出 / 无溢出 |
| `S` / `NS` | 符号标志置位 / 清零 | | |

- **`IndirectJump`**：经寄存器目标的间接跳转（生成器恢复、跳转表等）。

### 6.10 函数结构（6 条）

- **`Prologue`**：函数序言（callee-saved 保存、栈帧设置）。
- **`SetupFrame`**：建立 JIT 帧（frame header、与解释器帧的关联元数据）。
- **`Return`**：IR 级"函数值返回"终结指令（isTerminator）。
- **`EpilogueEnd`**：收尾标记（isTerminator）；**`Leave`**：恢复栈/寄存器；**`Ret`**：最终机器 `ret`。

### 6.11 生成器（5 条）

- **`YieldInitial`**：生成器初始挂起（创建生成器对象并返回）。
- **`StoreGenYieldPoint`** / **`StoreGenYieldFromPoint`**：记录 yield / yield-from 挂起点（恢复与 deopt 定位用）。
- **`BranchToYieldExit`**：跳转生成器退出路径（isTerminator）。
- **`ResumeGenYield`**：从挂起点恢复执行。

### 6.12 选择与合并（2 条）

- **`Select`**：`cmov` 风格三目选择（条件、真值、假值均需物理寄存器；输入跨指令存活；flag kInvalidate）。
- **`Phi`**：LIR 级 phi，输入成对出现（前驱块标签 + 值）；postalloc 阶段消除为 move 序列。

### 6.13 TreeIter 状态机（13 条）

与 HIR 同名指令一一对应（操作 JIT 帧 footer 中的 heap 状态结构），全部为 essential 副作用指令：

- **`EnsureTreeIterState`**：确保状态结构已分配（失败抛 MemoryError）。
- **`SaveCurrentNode`** / **`LoadCurrentNode`**：写 / 读（带 incref）当前树节点。
- **`SavePhase`** / **`LoadPhase`**：写 / 读当前阶段。
- **`StateStackPush`** / **`StateStackPop`**：显式栈压入 / 弹出 (node, phase)。
- **`LoadPoppedPhase`** / **`LoadStackTop`**：读最近弹出 phase / 栈顶。
- **`CheckTreeIterChildEntry`**：子节点进入前的门控检查（类型精确性、active-path、深度）。
- **`TreeIterEnterChild`** / **`TreeIterLeaveCurrentNode`**：进入 / 离开节点的簿记。
- **`ClearTreeIterState`**：释放全部持有引用。

## 7. 指令属性总表（113 条）

符号约定：标志 `–`=kNone、`Set`、`Inv`=kInvalidate；尺寸 `–`=kDefault、`64`=kAlways64、`Out`=kOut；`跨活`=inputs_live_across；`出物`=输出必须物理寄存器（`栈` 表示可分配栈槽）；`入物`=必须物理寄存器的输入下标（空白=无）；`ess`=is_essential。未标注即取默认值。

| 指令 | 标志 | 尺寸 | 跨活 | 出物 | 入物 | ess |
|---|---|---|---|---|---|---|
| Bind | – | – | – | ✓ | | – |
| Nop | – | – | – | ✓ | | – |
| Unreachable | – | – | – | 栈 | | ✓ |
| Call | Inv | 64 | – | ✓ | | ✓ |
| LoadAttrCachedFastPath | Inv | 64 | – | ✓ | | ✓ |
| VectorCall | Inv | 64 | – | ✓ | 0 | ✓ |
| VarArgCall | Inv | – | – | ✓ | 0 | – |
| Guard | Inv | – | – | ✓ | 2,3 | ✓ |
| DeoptPatchpoint | Inv | – | – | 栈 | 0,1 | ✓ |
| OSREntry | – | – | – | 栈 | | ✓ |
| Sext | – | – | – | ✓ | | – |
| Zext | – | – | – | ✓ | | – |
| Negate | Set | Out | – | ✓ | | – |
| Invert | – | Out | – | ✓ | | – |
| Add | Set | Out | – | ✓ | 0 | – |
| Sub | Set | Out | ✓ | ✓ | 0 | – |
| And | Set | Out | – | ✓ | 0 | – |
| Xor | Set | Out | – | ✓ | 0 | – |
| Div | Set | – | – | ✓ | 0 | – |
| DivUn | Set | – | – | ✓ | 0 | – |
| Mul | Set | Out | – | ✓ | 0 | – |
| MulAdd | – | 64 | – | ✓ | 0,1,2 | – |
| Or | Set | Out | – | ✓ | 0 | – |
| Fadd | – | 64 | – | ✓ | 0,1 | – |
| Fsub | – | 64 | ✓ | ✓ | 0,1 | – |
| Fmul | – | 64 | – | ✓ | 0,1 | – |
| Fdiv | – | 64 | ✓ | ✓ | 0,1 | – |
| Int64ToDouble | – | 64 | – | ✓ | 0 | – |
| LShift | Set | – | – | ✓ | | – |
| RShift | Set | – | – | ✓ | | – |
| RShiftUn | Set | – | – | ✓ | | – |
| Test | Set | – | – | 栈 | 0,1 | – |
| Test32 | Set | – | – | 栈 | 0,1 | – |
| Equal | Set | – | – | ✓ | 0,1 | – |
| NotEqual | Set | – | – | ✓ | 0,1 | – |
| GreaterThanSigned | Set | – | – | ✓ | 0,1 | – |
| LessThanSigned | Set | – | – | ✓ | 0,1 | – |
| GreaterThanEqualSigned | Set | – | – | ✓ | 0,1 | – |
| LessThanEqualSigned | Set | – | – | ✓ | 0,1 | – |
| GreaterThanUnsigned | Set | – | – | ✓ | 0,1 | – |
| LessThanUnsigned | Set | – | – | ✓ | 0,1 | – |
| GreaterThanEqualUnsigned | Set | – | – | ✓ | 0,1 | – |
| LessThanEqualUnsigned | Set | – | – | ✓ | 0,1 | – |
| Cmp | Set | Out | – | ✓ | 0,1 | – |
| Lea | – | 64 | – | ✓ | 0,1 | – |
| ReserveStack | – | 64 | – | ✓ | | – |
| LoadArg | – | 64 | – | ✓ | | – |
| LoadSecondCallResult | – | – | – | 栈 | | – |
| Exchange | – | 64 | – | ✓ | 0,1 | – |
| Move | – | Out | – | ✓ | | – |
| MoveRelaxed | – | Out | – | ✓ | | – |
| MovConstPool | – | Out | – | ✓ | | – |
| Push | – | – | – | ✓ | | ✓ |
| Pop | – | – | – | 栈 | | ✓ |
| Cdq | – | – | – | ✓ | | ✓ |
| Cwd | – | – | – | ✓ | | ✓ |
| Cqo | – | – | – | ✓ | | ✓ |
| Branch | – | – | – | ✓ | | – |
| BranchNZ | – | – | – | ✓ | | – |
| BranchZ | – | – | – | ✓ | | – |
| BranchA | – | – | – | ✓ | | – |
| BranchB | – | – | – | ✓ | | – |
| BranchAE | – | – | – | ✓ | | – |
| BranchBE | – | – | – | ✓ | | – |
| BranchG | – | – | – | ✓ | | – |
| BranchL | – | – | – | ✓ | | – |
| BranchGE | – | – | – | ✓ | | – |
| BranchLE | – | – | – | ✓ | | – |
| BranchC | – | – | – | ✓ | | – |
| BranchNC | – | – | – | ✓ | | – |
| BranchO | – | – | – | ✓ | | – |
| BranchNO | – | – | – | ✓ | | – |
| BranchS | – | – | – | ✓ | | – |
| BranchNS | – | – | – | ✓ | | – |
| BranchE | – | – | – | ✓ | | – |
| BranchNE | – | – | – | ✓ | | – |
| BitTest | Set | – | – | ✓ | 0 | – |
| Inc | Set | – | – | ✓ | | – |
| Dec | Set | – | – | ✓ | | – |
| CondBranch | Inv | – | – | 栈 | 0 | – |
| Select | Inv | – | ✓ | ✓ | 0,1,2 | – |
| Phi | – | – | – | ✓ | | – |
| Return | Inv | – | – | ✓ | | – |
| MovZX | – | – | – | ✓ | | – |
| MovSX | – | – | – | ✓ | | – |
| MovSXD | – | – | – | ✓ | | – |
| IntToBool | Set | – | – | ✓ | 0 | – |
| LoadThreadState | Inv | – | – | 栈 | | – |
| YieldInitial | Inv | – | – | 栈 | | ✓ |
| StoreGenYieldPoint | Inv | – | – | 栈 | | ✓ |
| StoreGenYieldFromPoint | Inv | – | – | 栈 | | ✓ |
| BranchToYieldExit | – | – | – | 栈 | | ✓ |
| ResumeGenYield | Inv | – | – | 栈 | | ✓ |
| EpilogueEnd | Inv | – | – | 栈 | | ✓ |
| Prologue | Inv | – | – | 栈 | | ✓ |
| SetupFrame | Inv | – | – | 栈 | | ✓ |
| IndirectJump | Inv | – | – | 栈 | | ✓ |
| VariadicPush | – | – | – | 栈 | | ✓ |
| Leave | Inv | – | – | 栈 | | ✓ |
| Ret | Inv | – | – | 栈 | | ✓ |
| EnsureTreeIterState | Inv | – | – | ✓ | | ✓ |
| SaveCurrentNode | Inv | – | – | 栈 | 0 | ✓ |
| LoadCurrentNode | – | 64 | – | ✓ | | ✓ |
| SavePhase | – | – | – | 栈 | 0 | ✓ |
| LoadPhase | – | – | – | ✓ | | – |
| StateStackPush | Inv | – | – | ✓ | 0,1 | ✓ |
| StateStackPop | – | 64 | – | ✓ | | ✓ |
| LoadPoppedPhase | – | – | – | ✓ | | – |
| LoadStackTop | – | – | – | ✓ | | – |
| CheckTreeIterChildEntry | Inv | – | – | ✓ | 0 | ✓ |
| TreeIterEnterChild | Inv | – | – | 栈 | 0 | ✓ |
| TreeIterLeaveCurrentNode | Inv | – | – | 栈 | | ✓ |
| ClearTreeIterState | Inv | – | – | 栈 | | ✓ |

## 8. HIR → LIR 下降示例

三条代表性路径（详见 `Jit/lir/generator.cpp`）：

1. **`BinaryOp<Add>`** → 查 `binaryfunc` 助手表得到 `PyNumber_Add` 地址 → `Call`（`MemImm` 函数指针 + 两个 VReg 实参）→ `Guard`（kNotZero 检查返回值非 NULL）。
2. **`VectorCall`** → 优先 `TranslateSpecializedCall`（被调方已编译时直接特化）；否则生成 LIR `VectorCall`：输入 0 为函数对象 VReg、输入 1 为 flags Imm、其余实参逐个附着。
3. **可 deopt 指令**（GuardType/GuardIs/Check* 等）→ `appendGuard` 生成 `Guard`，输入依次为 kind、deopt id、被查值、目标（对象/类型 MemImm）与全部 live 值。

## 9. 延伸阅读

- `cinderx/Jit/guide.md`：JIT 总体开发指南；
- 本仓库 `docs/guide/HIR指令指南.md`：HIR 指令全集；
- `cinderx/Jit/lir/regalloc.cpp` 文件头：线性扫描寄存器分配算法说明；
- `cinderx/Jit/deoptimization.md`：deopt 与 Guard 的运行时机制。
