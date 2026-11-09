# Whippet M2 后端：调用、栈帧与内存

> 作者：dhjs0000
>
> 对应实现：`src/isel/isel.c`、`src/regalloc/linear.c`、`src/emit/as.c`。
> M2 验收：**递归 fib 正确**（`fib_rec(10)=55`、`fib_rec(20)=6765`）。

## 1. 范围

M2 在 M1 的整数/控制流基础上增加：

- 函数调用：直接调用、间接调用（函数指针）、外部 C 函数（固定签名）、递归；
- Windows x64 栈参数：第 5 个及之后的整数/指针参数；
- 32 字节 shadow space 与调用点 16 字节栈对齐；
- 函数栈帧：参数 staging、spill、alloca、phi 边 scratch；
- 内存：`alloca`、`load`、`store`、`ptradd`（i8/i16/i32/i64/ptr）；
- 跨调用的指针（alloca 地址可传给被调方）。

M2 仍不支持浮点（M4）、可变参数、栈探测（>4096 字节帧）、异常/unwind、
尾调用优化。

## 2. ABI

| 项目 | 约定 |
| --- | --- |
| 整数/指针参数 | 前 4 个：RCX、RDX、R8、R9 |
| 栈参数 | 第 5 个起在 `[rsp+32+8*(i-4)]`（调用前） |
| 返回 | EAX / RAX（void 不设置） |
| shadow space | 调用方预留 32 字节 |
| 调用点对齐 | `rsp % 16 == 0`（执行 `call` 前） |
| 调用方保存 | RAX、RCX、RDX、R8–R11、XMM0–5 |
| 被调方保存 | RBX、RBP、RSI、RDI、R12–R15、XMM6–15 |

被调方入口 `rsp % 16 == 8`（返回地址已压栈）。M2 只用通用寄存器，不使用 XMM。

## 3. 栈帧布局

从 `rsp`（低地址）向高地址：

```
[rsp + 0]                 call area: 32 B shadow + max_stack_args*8
[rsp + call_area]         alloca 槽（每个 8 B）
[rsp + param_base]        参数 staging 槽（每个参数 8 B）
[rsp + spill_base]        spill 槽（每个 8 B）
[rsp + scratch_base]      phi 边 scratch（每个边最多 max_phi_moves 个）
...                       16 字节对齐 + 可能的 8 字节对齐填充
```

- `call_area_slots = has_calls ? 4 + max_stack_args : 0`；
- 每个 `alloca` 占一个 8 字节槽（M2 只有标量/指针 alloca，无数组计数）；
- 参数 staging 槽覆盖**所有**参数（寄存器参数也先落槽，避免并行搬移覆盖）；
- spill/phi scratch 沿用 M1 机制；
- 帧大小 16 字节对齐；若函数有调用且压栈的 callee-saved 寄存器数为偶数，
  再补 8 字节，使调用点 `rsp % 16 == 0`。

## 4. Prologue / Epilogue

Prologue：

1. `push` 实际用到的 callee-saved 寄存器（固定顺序 RBX、RBP、RSI、RDI、
   R12–R15）；
2. `sub rsp, frame_size`；
3. 前 4 个参数从 RCX/RDX/R8/R9 归一化后写入 staging 槽；
4. 第 5 个起从 `[rsp + 40 + 8*saved + frame_size + 8*(i-4)]` 读取；
   这个偏移把压栈和帧减去的空间加回去；
5. 被分配到寄存器的参数从 staging 槽装入 callee-saved 寄存器。

Epilogue：`add rsp, frame_size` → 逆序 `pop` → `ret`。返回值在 pop 之前
装入 RAX（返回值不会被 pop 覆盖）。

## 5. 调用约定与寄存器分配

- 含调用的函数，活跃值只分配到 **callee-saved** 寄存器或栈槽；调用方保存的
  R8–R11 只作为调用参数/临时寄存器使用。这样跨调用活跃值不会被 callee 破坏。
- 不含调用的叶函数仍可使用全部 12 个可分配寄存器（R8–R11 + 8 个 callee-saved）。
- 调用序列：
  1. 先写栈参数：值装入 RAX，再存到 `[rsp+32+8*(i-4)]`；
  2. 再装前 4 个寄存器参数到 RCX/RDX/R8/R9；
  3. 直接调用 `call @name`；间接调用把函数指针装入 RAX 后 `call rax`；
  4. 非 void 返回值按结果类型归一化（i8/i16/i32 截断）后写入结果位置。
- 因为活跃值不在 RCX/RDX/R8/R9 中，参数装载不会互相覆盖，无需并行搬移。
- 直接调用仍由 M0 解析器核对声明/定义的参数个数、类型和返回类型；
  间接调用使用 IR 中写的参数类型，不核对函数指针签名。

## 6. 内存

`alloca`：

```
lea rax, [rsp + alloca_offset]
```

- 每个 alloca 一个 8 字节槽；指针指向该槽；
- 地址在函数生命周期内有效，可以跨调用传递（被调方通过指针读写调用方栈）。

`load`：

```
mov rcx, <指针>
movzx eax, byte/word ptr [rcx]   ; i8/i16
mov eax, dword ptr [rcx]         ; i32
mov rax, qword ptr [rcx]         ; i64/ptr
```

`store` 反向写 `al/ax/eax/rax`。i8/i16/i32 的值在寄存器里始终以零扩展的
规范形式保存；有符号使用处再按类型宽度符号扩展。

`ptradd`：基址装入 RAX，偏移装入 RCX 后按偏移类型符号扩展，再 `add rax, rcx`。
因此负偏移可用。

## 7. 栈帧与对齐示例

`fib_rec` 有 4 个 callee-saved 寄存器、32 字节 shadow、无栈参数：

```
push rbx; push rbp; push rsi; push rdi   ; 4 次 push
sub rsp, 56                              ; 32 call area + 8 param + 8 padding + align
...
call fib_rec
...
add rsp, 56
pop rdi; pop rsi; pop rbp; pop rbx
ret
```

入口 `rsp%16=8`，4 次 push 后仍为 8，减 56（56%16=8）后为 0，满足调用点对齐。

`sum5` 的第 5 个参数从 `[rsp+120]` 读取（4 次 push + 48 字节帧 + 40 的入口偏移）。

## 8. 测试

Windows/Clang CTest：

- `asm_m2_calls`：递归 fib、C→IR 栈参数（5/6 参数）、IR→IR 栈参数、
  参数转发、间接函数指针调用、CRT `putchar` 外部调用、i64 参数/返回、
  i8 返回归一化；
- `asm_m2_memory`：alloca、load/store、ptr 跨调用、i8/i16/i32/i64 宽度；
- 既有 `asm_add_mul`、`asm_sum_to`、`asm_int_ops`、`asm_switch` 继续通过；
- `asm_differential` 随机整数 IR vs C 继续通过；
- `examples` 增加 `fib_rec.wir`、`pointers.wir`、`hello.wir`。

## 9. 后续

- **M3**：内部汇编器 + COFF 直写、O1 中端流水线、编译速度；
- **M4**：浮点/XMM、struct/union、-O2；
- 后续可选：栈探测、unwind info、跳转表、尾调用、可变参数。
