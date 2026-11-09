# Whippet 示例（M2/M3）

这些示例使用当前后端：整数子集、分支/`switch`/phi/循环、函数调用、
栈参数、递归、alloca/load/store/指针。CLI 默认走 O1；脚本用 `-S` 输出
Windows x64 GNU as 文本，也可以按 `docs/M3.md` 用 `-c` 直接生成 COFF。
需要先按仓库根目录的 README 构建 `build/whippet.exe`。

## 一键运行（Windows）

在仓库根目录的 PowerShell 中：

```powershell
powershell -ExecutionPolicy Bypass -File .\examples\run_examples.ps1
```

脚本会尝试自动加载 Visual Studio 的 `vcvars64.bat`（供 clang 链接 CRT），
然后依次生成汇编、汇编链接、运行并检查输出。

## 手动运行单个示例

```powershell
# add_mul: 生成汇编 -> 与 C driver 链接 -> 运行
New-Item -ItemType Directory -Force .\build\examples | Out-Null
.\build\whippet.exe -S -o .\build\examples\add_mul.s .\examples\add_mul.wir
clang .\examples\add_mul_driver.c .\build\examples\add_mul.s -o .\build\examples\add_mul.exe
.\build\examples\add_mul.exe

# 或者内部汇编器直出 COFF
.\build\whippet.exe -c -o .\build\examples\add_mul.obj .\examples\add_mul.wir
clang .\examples\add_mul_driver.c .\build\examples\add_mul.obj -o .\build\examples\add_mul_obj.exe
.\build\examples\add_mul_obj.exe
```

若链接时报找不到 `link.exe`，先在 “x64 Native Tools Command Prompt for VS”
里运行，或先 `call` 对应的 `vcvars64.bat`。

## 示例列表

| 文件 | 内容 | 期望输出 |
| --- | --- | --- |
| `add_mul.wir` | 设计文档的 `a*4+b` | `add_mul(6, 7) = 31` |
| `fib.wir` | 迭代 fib，含 phi/循环 | `fib(0..3,10) = 0,1,1,2,55` |
| `fib_rec.wir` | **递归 fib，函数调用 + 栈帧** | `fib_rec(20) = 6765` |
| `classify.wir` | 条件分支 + 位置式 switch | `0,5,20,30,30` |
| `bitops.wir` | 算术、移位、比较、select | `100,144,100` |
| `pointers.wir` | alloca/load/store、指针跨调用 | `use_ptr=42, memory=42` |
| `hello.wir` | 自包含 `main`，调用 CRT `putchar` | 输出 `Hi`，退出码 0 |
| `exit42.wir` | 自包含 `main` | 进程退出码 `42` |

`classify.wir` 的 switch 是**位置式**的：case 索引 0 跳到 `case0`，索引 1
跳到 `case1`，其余走 `other`。M1 暂时用比较链降低 switch，跳转表是后续优化。

`exit42.wir` 不需要 driver，可以单独验证：

```powershell
.\build\whippet.exe -S -o .\build\examples\exit42.s .\examples\exit42.wir
clang .\build\examples\exit42.s -o .\build\examples\exit42.exe
.\build\examples\exit42.exe
$LASTEXITCODE   # 42
```

## 当前限制

M2 已支持函数调用（含递归/间接调用/栈参数）和 `alloca/load/store/ptradd`；
仍不支持浮点、可变参数和超过 4096 字节的栈帧（栈探测尚未实现）。这些示例
都只使用当前可用子集。更完整的后端说明见
[../docs/M2_ABI.md](../docs/M2_ABI.md)。
