# Whippet

Ethernos Studio 的独立编译器后端。接收前端产出的 Whippet IR，负责中端优化与
x86-64 代码生成。设计见 [design/WHIPPET_DESIGN.md](design/WHIPPET_DESIGN.md)，
M0 脚手架见 [design/WHIPPET_M0_SCAFFOLD.md](design/WHIPPET_M0_SCAFFOLD.md)。

作者：dhjs0000

许可证：[MIT](LICENSE)。

当前仓库已完成 **M0 + M1 + M2**：

- **M0**：Whippet IR 文本（`.wir`）的完整词法/语法解析；严格校验未知
  opcode/类型/谓词、重复定义、未定义引用、phi 位置、块终止、类型与 cast
  合法性、直接调用签名；canonical 打印器逐字节 round-trip；
- **M1**：naive 指令选择 + 线性扫描寄存器分配 → Windows x64 GNU as 文本
  （Intel 语法），覆盖整数子集、分支/`switch`（临时比较链）/phi/循环、
  寄存器溢出、被调方保存寄存器；验收 `add_mul`、`sum_to`、`switch` 和整数综合
  用例均汇编/链接/运行正确；
- **M2**：函数调用（直接/间接/递归/外部 C）、Windows x64 栈参数、
  shadow space 与 16 字节调用点对齐、完整栈帧、`alloca/load/store/ptradd`、
  指针跨调用；验收递归 `fib_rec(10)=55`、`fib_rec(20)=6765`，栈参数、
  间接调用和内存用例全部汇编/链接/运行正确；
- arena + 24 B IR 节点 + u32 句柄（符合 §9.2 内存预算）；
- `ctest` 覆盖 IR round-trip、20 个错误定位、CLI、以及端到端汇编/链接/运行。

## 目录

```
whippet/
├── CMakeLists.txt
├── include/whippet/        # 公共 API：ir.h、emit.h、codegen.h
├── src/
│   ├── ir/                 # M0：arena、节点池、解析、校验、打印
│   ├── mid/passes.c        # M3：-O1 pass 流水线（占位）
│   ├── isel/isel.c         # M1/M2：指令选择 + 调用/栈帧/内存 + GNU as 文本
│   ├── regalloc/linear.c   # M1/M2：线性扫描、phi 边、调用感知分配
│   ├── emit/as.c           # 文本/汇编发射入口
│   └── cli/main.c          # whippet 命令行
├── tests/                  # IR/诊断/汇编测试 + canonical 用例
├── examples/               # 可直接汇编/链接/运行的 M2 示例 + 一键脚本
├── docs/IR_TEXT.md         # IR 文本契约（语法参考）
├── docs/M1_BACKEND.md      # M1 后端：ABI、寄存器、指令选择、限制
├── docs/M2_ABI.md          # M2：调用、栈帧、内存、ABI
└── design/                 # 设计文档随仓库走
```

## 构建

编译器统一用 Clang，生成器用 Ninja（与设计 §11 一致）。

### Arch / Linux

```bash
sudo pacman -S --needed cmake ninja clang
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows

先进入 Visual Studio 开发者环境（提供 Windows SDK / MSVC 链接器；编译器仍用
clang），再配置。Ninja 若不在 `PATH`，用 `-DCMAKE_MAKE_PROGRAM=<ninja.exe>` 指定：

```powershell
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build
ctest --test-dir build --output-on-failure
```

本机（Visual Studio 装在 `E:\VisualStudio`、LLVM 在 `E:\LLVM`、Ninja 由 VS 附带）
可用：

```powershell
cmd /c "call E:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat && E:\Python\Python315\Scripts\cmake.exe -B build -G Ninja -DCMAKE_C_COMPILER=E:/LLVM/bin/clang.exe -DCMAKE_MAKE_PROGRAM=E:/VisualStudio/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
cmd /c "call E:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat && E:\Python\Python315\Scripts\cmake.exe --build build"
cmd /c "E:\Python\Python315\Scripts\ctest.exe --test-dir build --output-on-failure"
```

### Sanitizer（合并前必跑）

```bash
cmake -B build-asan -G Ninja -DCMAKE_C_COMPILER=clang -DWHIPPET_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Windows 下 CMake 会把 Clang 的 `clang_rt.asan_dynamic-x86_64.dll` 自动复制到
可执行文件旁边，无需改 `PATH`。

## 命令行

```text
whippet [-S|--emit-ir] [-o <file>] [--check] <file.wir>
```

- 默认（`-S` / `--emit-asm`）：解析、校验、生成 Windows x64 GNU as 文本；
- `--emit-ir`：打印 canonical IR（M0 调试路径）；
- `-o <file>`：写文件而不是 stdout（二进制写，保证 LF）；
- `--check`：只解析校验；
- 出错时输出 `path:line:column: error: message`，并打印对应源码行与 `^`。

```console
$ whippet tests/cases/basic.wir
.intel_syntax noprefix
.text

.globl add_mul
add_mul:
  sub rsp, 16
  ...
  ret
```

## 示例

[examples/](examples) 下有一组可直接汇编/链接/运行的 M2 示例（add_mul、
迭代/递归 fib、switch、位运算、指针/内存、CRT hello，以及 exit42）。
Windows 下一条命令：

```powershell
powershell -ExecutionPolicy Bypass -File .\examples\run_examples.ps1
```

脚本会自动加载 Visual Studio 环境（供 clang 链接 CRT），逐个生成汇编、链接、
运行并校验输出。手动命令与每个示例的期望输出见
[examples/README.md](examples/README.md)。

## 测试

```console
$ ctest --test-dir build --output-on-failure
...
100% tests passed, 0 tests failed out of 15
```

Windows/Clang 配置下共 15 项；GCC/MinGW 构建会跳过 8 项 Windows ABI
端到端测试（仍保留 IR/汇编文本检查）。

- `tests/cases/*.wir` 以 canonical 形式保存，round-trip 必须逐字节一致；
- `test_ir_errors` 断言非法输入被拒绝，并核对诊断行列；
- `test_ir_metadata` 检查 opcode/类型/谓词表自洽与空模块；
- `test_ir_normalize` 验证 BOM/注释/CRLF/十六进制/指数浮点的归一化；
- `test_asm` 检查汇编生成与“未支持类型”的定位诊断；
- `cli_emit` 端到端验证 `--emit-ir` 写出的文件与 canonical 用例一致；
- `asm_add_mul` / `asm_sum_to` / `asm_int_ops` / `asm_switch`：Windows/Clang
  下生成汇编、汇编链接 C driver、运行并比对期望值；
- `asm_m2_calls`：递归、栈参数、间接调用、外部 CRT 调用；
- `asm_m2_memory`：alloca/load/store、指针跨调用、i8/i16/i32/i64 宽度；
- `asm_differential`：随机整数 IR vs 等价 C，经 whippet 与 clang 分别编译
  后逐输入比对（Windows/Clang + Python）；
- `examples`：运行 `examples/` 下的完整示例，验证一键脚本与示例输出。

## IR 文本契约

M0 实现的完整语法、类型规则、canonical 格式与已知边界见
[docs/IR_TEXT.md](docs/IR_TEXT.md)。要点：

- 七种操作数类型 `i8/i16/i32/i64/f32/f64/ptr`，`void` 仅用于返回值；
- 无 `i1`：比较产生 `i32` 0/1，`condbr` 对任意非零整数为真；
- SSA、显式基本块、`phi` 仅出现在块首；
- 跳转表 `switch` 是一等指令；
- 前端负责 SSA 构建、类型降级、控制流展平、语义裁决与聚合返回值（§3.2）；
- 解析器只做“海关”：拒绝畸形输入，不做前端语义的补救。

## 后端

目标平台为 **Windows x64 + MSVC ABI**；naive 指令选择 + 线性扫描，生成
GNU as（Intel 语法）文本。

- M1：整数/控制流、phi、spill、callee-saved、switch 比较链；
  ABI 账本、寄存器池、活跃性/phi 边处理见
  [docs/M1_BACKEND.md](docs/M1_BACKEND.md)；
- M2：函数调用（直接/间接/递归/外部 C）、栈参数、shadow space、调用点
  16 字节对齐、完整栈帧、`alloca/load/store/ptradd`、指针跨调用；
  细节见 [docs/M2_ABI.md](docs/M2_ABI.md)。

当前仍明确不支持（会给出 `path:line:column` 诊断）：全部浮点、可变参数、
超过 4096 字节的栈帧（栈探测尚未实现）、unwind info。浮点排到 M4；
`switch` 已用比较链正确降级，跳转表发射留待后续。

## 工程纪律

1. IR 存储一律走 arena（`WhippetArena`），节点/操作数/名字均不散落 `malloc`；
2. `-DWHIPPET_SANITIZE=ON` 下测试全绿才算完成；
3. 差分测试从 M1 起建：[tests/diff_test.py](tests/diff_test.py) 生成随机整数
   IR 与等价 C，经 whippet/clang 分别编译后逐输入比对；固定用例还覆盖
   phi 交换环、spill、callee-saved、递归、栈参数、间接调用和内存。

M0 的解析器检查 SSA 名字/块引用的存在性与类型一致性；**支配关系**
（dominance）仍由前端保证，后端不重建 SSA。phi 前驱边检查由后端完成。
