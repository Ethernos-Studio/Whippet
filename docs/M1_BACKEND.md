# Whippet M1 后端

> 作者：dhjs0000
>
> 对应实现：`src/isel/isel.c`、`src/regalloc/linear.c`、`src/emit/as.c`。
> 目标：naive 指令选择 → GNU as（Intel 语法）文本，整数子集；验收为
> `add_mul` 汇编、链接、运行正确。

## 1. 目标平台与 ABI

M1 只生成 **Windows x64（MSVC ABI）** 代码：

| 项目 | 约定 |
| --- | --- |
| 参数寄存器 | RCX、RDX、R8、R9（最多 4 个；M2 加栈参数） |
| 返回寄存器 | EAX / RAX（void 不设置） |
| 调用方保存 | RAX、RCX、RDX、R8–R11、XMM0–5 |
| 被调方保存 | RBX、RBP、RSI、RDI、R12–R15 |
| 栈 | 叶函数；帧大小按 16 字节对齐；不使用红区 |

M1 不生成函数调用，因此不需要 shadow space、栈参数和 16 字节调用点对齐；
这些在 M2 补齐。SysV 是后续移植，不在 M1 范围。

汇编文本使用：

```asm
.intel_syntax noprefix
.text
.globl add_mul
add_mul:
  ...
```

该文本可直接交给 clang/LLVM 集成汇编器、`llvm-mc` 或 MinGW `as`。

## 2. 流水线

```
Whippet IR
  -> 支持性校验（M1 子集 / ABI 限制）
  -> 计算 phi 边最大移动数
  -> 线性扫描寄存器分配
  -> 构建 phi 边块（临界边拆分）
  -> 逐指令 naive 选择 + 文本发射
```

寄存器分配在发射前完成；因此所有 value 都有确定位置（寄存器或栈槽），
发射阶段不需要回填。

## 3. 寄存器与栈槽

- **固定 scratch**：RAX、RCX、RDX。值永远不会分配到这里。
  - RAX：主计算 / 返回值；
  - RCX：第二操作数 / 移位计数（CL）/ 除数；
  - RDX：`cmov` 临时值、除法高位。
- **可分配寄存器**：R8、R9、R10、R11、RBX、RBP、RSI、RDI、R12–R15。
  - 优先使用 caller-saved 的 R8–R11，减少压栈；
  - 用到 callee-saved 时在 prologue `push`、epilogue `pop`。
- **栈槽**：每个 8 字节，`[rsp + offset]`。布局：
  1. 参数 staging 槽（每个参数一个，即使被分配到寄存器也保留）；
  2. spill 槽；
  3. phi 边移动 scratch 槽（每个边最多 `max_phi_moves` 个，复用）。
- 帧大小 16 字节对齐；M1 限制 4096 字节，避免需要 Windows 栈探测
  （`__chkstk` 属于 M2）。

## 4. 活跃区间与线性扫描

活跃区间不是简单的“文本顺序 def—last use”：

- 普通 use 记在指令位置；
- phi 的入边 use 记在**前驱块末尾**（因此允许前向引用）；
- 再做一次 CFG 反向活跃性分析，把 live-in/live-out 的值扩展到整个块，
  这样循环头使用的值（例如 `%n`）不会被循环体里的新值覆盖。

扫描按 start 排序，维护按 end 排序的 active 表；寄存器不足时溢出 end
最远的区间。phi 结果强制放在栈槽，避免并行复制环。

## 5. Phi 降低

M1 采用“phi 结果栈驻留 + 边块”的方式：

1. 每个含 phi 的目标块，对每个前驱边创建一个合成边块（必要时拆分临界边）；
2. 边块里先把所有入边值快照到 scratch 槽，再写入 phi 结果槽；
3. 这样即使 phi 之间形成交换环（`a←b, b←a`）也不会互相覆盖；
4. 边块最后 `jmp` 到目标块。

`phi` 指令本身在目标块中不生成代码；后续使用 phi 结果时直接从栈槽读取。

## 6. 指令选择

所有整数计算统一在 64 位寄存器中做，然后按 IR 类型归一化到低位宽：

| 类型 | 归一化 |
| --- | --- |
| i8 | `movzx eax, al` |
| i16 | `movzx eax, ax` |
| i32 | `mov eax, eax`（写 32 位自动清高 32 位） |
| i64 / ptr | 无 |

有符号操作前从类型宽度做符号扩展（`movsx` / `movsxd`）：

- `icmp` 的有序谓词（slt/sle/sgt/sge）；
- `sdiv` / `srem`；
- `sext`；
- `ashr` 使用类型宽度的 `sar al/ax/eax/rax, cl`。

主要映射：

| IR | 汇编 |
| --- | --- |
| `add/sub/and/or/xor` | `add/sub/and/or/xor rax, rcx` |
| `mul` | `imul rax, rcx` |
| `sdiv/srem` | `cqo; idiv rcx`，余数取 RDX |
| `udiv/urem` | `xor edx, edx; div rcx`，余数取 RDX |
| `shl/lshr/ashr` | 按类型宽度的 `shl/shr/sar ..., cl` |
| `neg/not` | `neg/not rax` |
| `icmp` | `cmp` + `setcc al` + `movzx eax, al` |
| `select` | `test rcx, rcx; cmovne rax, rdx` |
| `switch` | 临时 `cmp rax, case; je ...` 比较链（跳转表后续） |
| `copy` | load + store |
| `ret` | 值装入 RAX，然后 epilogue |

转换：`trunc` 归一化到目标宽度；`zext` 直接使用零扩展表示；`sext` 先
符号扩展再归一化；`bitcast`（同宽整数/指针）、`ptrtoint`、`inttoptr` 支持。

## 7. M1 支持范围

**支持**：`add sub mul sdiv udiv srem urem and or xor shl lshr ashr neg not`、
`icmp`、`phi`、`copy`、`select`、`trunc zext sext bitcast ptrtoint inttoptr`、
`br`、`condbr`、`switch`（临时比较链）、`ret`、整数/指针参数（≤4）与返回值、
寄存器溢出（spill）、被调方保存寄存器。

**明确拒绝并给出定位诊断**：

- `call`、`alloca`、`load`、`store`、`ptradd` → M2；
- 浮点 op、`fcmp`、浮点参数/返回/转换 → M4；
- 超过 4 个参数、栈使用超过 4096 字节 → M1 限制；
- 含 phi 的 entry block（保守限制）；
- phi 缺少某前驱的入边（不合法 SSA）。

## 8. 验收

Windows/Clang 配置下 CTest 会执行：

- `asm_add_mul`：生成汇编 → clang 汇编/链接 C driver → 运行，期望 31；
- `asm_sum_to`：phi/循环，期望 55；
- `asm_int_ops`：算术、移位、有符号/无符号除法、比较、select、cast、
  i8/i16/i32/i64、spill、callee-saved、phi 交换环，逐项与 C 期望比对；
- `asm_switch`：条件分支 + switch 比较链；
- `asm_differential`：随机整数 IR vs 等价 C（Windows/Clang + Python）。

```console
$ whippet tests/cases/basic.wir
.intel_syntax noprefix
.text

.globl add_mul
add_mul:
  ...
```

## 9. 后续

- **M2**：函数调用、栈参数、shadow space、栈探测、内存操作；
- **M3**：内部汇编器 + COFF 直写、O1 中端流水线、编译速度；
- **M4**：浮点/XMM、struct/union 布局、-O2。
