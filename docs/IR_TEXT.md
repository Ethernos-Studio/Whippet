# Whippet IR 文本格式（M0 实现）

> 对应实现：`src/ir/parse.c`、`src/ir/print.c`。
> 设计依据：[WHIPPET_DESIGN.md §3–§4](../design/WHIPPET_DESIGN.md)。
> 文本形式是前后端之间稳定的外部契约；本文件记录 M0 实际接受的语法与
> canonical 输出规则。

## 1. 词法

- **空白**：空格、制表符、CR、LF、换页；除换行外无意义。
- **注释**：`;` 或 `//` 到行尾。注释不进入 IR，打印时丢弃。
- **标识符**：首字符为字母、`_`、`.`、`$`，后续可接数字。区分大小写。
  - 值（虚拟寄存器）：`%name`，允许 `%0` 这类数字开头的名字；
  - 函数/全局符号：`@name`；
  - 基本块标签：裸标识符（不带 `%`），定义形如 `entry:`。
- **整数**：十进制，或 `0x` 十六进制；可带前导 `-`。
  取值范围为 `[-2^63, 2^63-1]`，超出报错。
- **浮点**：十进制，必须带小数点或指数，例如 `1.5`、`-0.25`、`0.0`、`1e-3`。
  不接受 `inf`/`nan` 字面量。
- **标点**：`{ } ( ) [ ] , : = ->`。
- **BOM**：文件开头的 UTF-8 BOM 会被跳过。
- **行尾**：解析器接受 CRLF/LF；canonical 输出统一为 LF。

## 2. 类型

```
void  i8  i16  i32  i64  f32  f64  ptr
```

- 操作数只允许后七种；`void` 仅用于函数返回类型和 `call` 的返回类型。
- 没有 `i1`。`icmp`/`fcmp` 的结果类型固定为 `i32`（0/1）；
  `condbr` 把任意非零整数视为真。
- `ptr` 在 Windows x64 数据模型下为 8 字节；`i32` 为 4 字节。
- 没有 `null` 字面量；空指针用 `inttoptr` 或前端自行约定表达（M0 未提供）。

## 3. 模块与函数

模块是顶层项的序列，顺序保留：

```
declare <ret-type> @name(<params>)

func @name(<params>) [-> <ret-type>] {
<label>:
  ...
}
```

- `<params>`：逗号分隔的 `<type> %name`，可为空。参数必须命名。
- 定义函数省略 `-> <ret-type>` 表示返回 `void`。
- 声明没有函数体；`declare void @abort()` 合法。
- 函数体内至少一个基本块；每个块必须以终结指令（`ret`/`br`/`condbr`/
  `switch`）结束。
- `phi` 只能出现在块首（连续多个 `phi` 可以）。
- 值名在函数内唯一；参数也是值。块标签在函数内唯一。

## 4. 指令

通用形式：`%result = <opcode> ...`。`store`/`br`/`condbr`/`switch`/`ret`
不产生结果；`call` 可以产生结果，也可以丢弃非 void 返回值（见下）。

值操作数写作 `%name`；常量按上下文类型书写（整数类型用整数字面量，浮点类型
用浮点字面量）。二元/一元指令的类型前缀同时约束操作数与结果，因此第二个
操作数不重复写类型。

### 4.1 终结指令

| 语法 | 说明 |
| --- | --- |
| `ret` | 仅用于返回 `void` 的函数 |
| `ret <ty> <val>` | 返回一个值；类型必须等于函数返回类型 |
| `br <block>` | 无条件跳转 |
| `condbr <int-ty> <val>, <block-true>, <block-false>` | 条件跳转；非零为真 |
| `switch <int-ty> <val>, [<block>, ...], <block-default>` | 跳转表；case 可为空 |

### 4.2 整数运算 / 位运算

```
%r = add|sub|mul  <ty> <val>, <val>
%r = sdiv|udiv    <ty> <val>, <val>
%r = srem|urem    <ty> <val>, <val>
%r = and|or|xor   <ty> <val>, <val>
%r = shl|lshr|ashr <ty> <val>, <val>
%r = neg|not      <ty> <val>
```

`<ty>` 必须是 `i8`/`i16`/`i32`/`i64`，结果类型与操作数类型相同。
算术按定长回绕，没有 `nsw/nuw` 标志。

### 4.3 浮点运算

```
%r = fadd|fsub|fmul|fdiv <ty> <val>, <val>
%r = fneg <ty> <val>
```

`<ty>` 必须是 `f32`/`f64`。没有 `frem`；取模由前端调用库函数表达。

### 4.4 比较

```
%r = icmp <pred> <ty> <val>, <val>     ; 结果 i32
%r = fcmp <pred> <ty> <val>, <val>     ; 结果 i32
```

`icmp` 谓词：`eq ne slt sle sgt sge ult ule ugt uge`。
`ptr` 只支持 `eq`/`ne`。

`fcmp` 谓词：`oeq one olt ole ogt oge ord uno ueq une ult ule ugt uge`。

### 4.5 内存

```
%p = alloca <ty>                       ; 结果 ptr
%v = load <ty>, ptr <val>              ; 结果 <ty>
     store <ty> <val>, ptr <val>       ; 无结果
%q = ptradd ptr <val>, <int-ty> <val>  ; 结果 ptr，按字节偏移
```

`ptradd` 不做元素大小缩放；前端负责把数组/结构体地址计算展开成字节偏移。

### 4.6 SSA 辅助

```
%r = phi <ty> [<val>, <block>], [<val>, <block>], ...
%r = copy <ty> <val>
%r = select <cond-ty> <val>, <ty> <val>, <ty> <val>
```

- `phi` 的每个入边是“值, 前驱块标签”；至少一个入边。
- `copy` 供中端复写传播使用；M1 指令选择会消除大部分 copy。
- `select` 的条件为整数类型；两个候选值类型必须相同，也是结果类型。
  注意类型重复写在每个操作数前。

### 4.7 调用

```
%r = call <ret-ty> @callee(<ty> <arg>, ...)
%r = call <ret-ty> %function_pointer(<ty> <arg>, ...)
     call void @callee(...)             ; 无结果
     call i32 @callee(...)              ; 允许丢弃非 void 返回值
```

- 直接调用 `@name` 会与模块中的声明/定义核对参数个数、参数类型和返回类型；
  可以调用后面才定义的函数。
- 间接调用 `%fp` 要求 `%fp` 类型为 `ptr`；M0 不核对函数指针签名。
- 不支持可变参数；`va_arg` 不属于 v1.0 子集。

### 4.8 类型转换

```
%r = trunc   <src-ty> <val> to <dst-ty>
%r = zext    <src-ty> <val> to <dst-ty>
%r = sext    <src-ty> <val> to <dst-ty>
%r = fpext   <src-ty> <val> to <dst-ty>
%r = fptrunc <src-ty> <val> to <dst-ty>
%r = bitcast <src-ty> <val> to <dst-ty>
%r = ptrtoint <src-ty> <val> to <dst-ty>
%r = inttoptr <src-ty> <val> to <dst-ty>
%r = sitofp  <src-ty> <val> to <dst-ty>
%r = uitofp  <src-ty> <val> to <dst-ty>
%r = fptosi  <src-ty> <val> to <dst-ty>
%r = fptoui  <src-ty> <val> to <dst-ty>
```

合法性：`trunc` 整数降宽；`zext`/`sext` 整数升宽；`fpext` f32→f64；
`fptrunc` f64→f32；`bitcast` 等宽且非 void；`ptrtoint` ptr→整数；
`inttoptr` 整数→ptr；`sitofp`/`uitofp` 整数→浮点；`fptosi`/`fptoui`
浮点→整数。

## 5. Canonical 输出

打印器不保留输入排版，而是输出唯一规范形式：

- 顶层项（函数/声明）之间一个空行；
- 块标签顶格，指令缩进两个空格；
- 块之间不空行；
- 每条指令一行，文件以一个 LF 结尾；
- 注释、多余空行、CRLF 被丢弃/归一化；
- 整数输出十进制；浮点输出能精确回读的最短十进制，并在需要时补 `.0`；
- 函数定义形如 `func @f(i32 %a) -> i32 {`；void 返回省略箭头；
- 声明形如 `declare i32 @putchar(i32 %c)`。

因此 `parse(print(module))` 与 `print(module)` 逐字节一致；`tests/cases/*.wir`
就是按此规则保存的。对非 canonical 输入（注释、额外空格、十六进制整数、
`1e-3` 等）解析仍成功，但打印会归一化——round-trip 保证针对 canonical 文本。

## 6. 校验清单

解析阶段会拒绝：

1. 未知 opcode、类型名、比较谓词；
2. 重复的函数名、参数名、值名、块标签；
3. 使用未定义的值或块（允许前向引用，例如 `phi` 引用后续块定义的值）；
4. 操作数类型与指令要求不符；
5. 非法的 cast 组合；
6. `phi` 不在块首、块缺少终结指令、终结指令后还有指令；
7. 空函数体 / 空块；
8. 直接调用未声明函数、参数个数/类型/返回类型不匹配；
9. 语法错误（缺逗号、括号不匹配等）。

**不检查**：SSA 支配关系、`phi` 入边与前驱集合的一致性、可达性。这些属于
前端职责（设计 §3.2），后端不重建 SSA。

## 7. M0 边界与后续

M0 文本契约暂不包含：

- 全局变量/符号表段（M2 与 ABI/重定位一起定）；
- 函数属性、调用约定标注、元数据、调试位置；
- 字符串字面量与重定位；
- 可变参数；
- 显式对齐（`alloca`/`load`/`store` 的 alignment 应在前端按类型保证）。

这些是后续里程碑在文本契约上的增量；新增 opcode 时保持“不认识的 opcode
报错而不是跳过”的原则（§4.3）。
