# 13 — 原生调用桥（as_callfunc：VM 栈 ↔ C++ ABI）

> **难度**：★★★★★（全系列最"硬核"的模块：手工实现 C++ 编译器的调用约定，含汇编）
> **前置**：第 11 篇（虚拟机执行循环——理解 VM 栈布局与 asBC_CALLSYS 指令）
>
> 本篇精读 as_callfunc.cpp/.h（公共层）、as_callfunc_x64_msvc.cpp + as_callfunc_x64_msvc_asm.asm
> （Windows x64）、as_callfunc_x64_gcc.cpp（System V x64 对比）、as_generic.cpp/.h（generic 约定）。

## 1. 模块定位

`engine->RegisterGlobalFunction("float sin(float)", asFUNCTION(sinf), asCALL_CDECL)` 传进来的
只是一个裸函数指针。脚本调用它时，参数躺在 VM 的 DWORD 数组栈上（第 11 篇），而 `sinf` 期望
参数出现在 XMM0 寄存器里——**谁来搬运？** 没有编译器帮忙（函数签名在运行期才知道），引擎只能
在运行时手工复刻 C++ 编译器的 calling convention 逻辑：把 VM 栈上的原始字节按目标平台 ABI
的规则装配到寄存器和 C++ 栈上，`call` 过去，再从 RAX/XMM0 里把返回值取回 VM。

调用约定是 CPU × OS × 编译器 三元组的函数，所以每种组合一份实现，光看文件名就能感受移植矩阵：

```
as_callfunc_x86.cpp              // 32位 x86（cdecl/stdcall/thiscall 全家桶，1514 行）
as_callfunc_x64_msvc.cpp/.asm    // Windows x64 (MSVC + MASM)
as_callfunc_x64_gcc.cpp          // Linux/Mac x64 (System V, 内联汇编)
as_callfunc_x64_mingw.cpp        // Windows x64 (MinGW)
as_callfunc_arm.cpp + 4 份汇编   // ARM32 (gcc/msvc/xcode/vita 各一份 .S/.asm)
as_callfunc_arm64.cpp + 3 份汇编 // ARM64
as_callfunc_riscv64.cpp / e2k.cpp / mips.cpp / ppc.cpp / ppc_64.cpp / sh4.cpp / xenon.cpp
```

公共层 as_callfunc.cpp 负责平台无关的部分；各平台文件只实现一个函数
`CallSystemFunctionNative()`（声明见 as_callfunc.cpp:602）。选择哪个文件参与编译由
as_config.h 里的平台探测宏（如 `AS_X64_MSVC`，as_config.h:521）决定。

## 2. 注册期的预计算：PrepareSystemFunction

调用约定的判定逻辑很贵，不能每次调用都做，所以引擎把它拆成两步：**注册期分析一次，存进
`asSSystemFunctionInterface`（as_callfunc.h:98-171）**；调用期只查表。关键字段：

| 字段 | 含义 |
|---|---|
| `func` / `auxiliary` | 函数指针 / 辅助对象指针（THISCALL_ASGLOBAL 的 this 等） |
| `callConv` | 内部约定枚举 `internalCallConv`（as_callfunc.h:70-96），注意每种约定都有一个 `_RETURNINMEM` 变体，且**恰好 = 基础值 +1**（后面会用到这个技巧） |
| `hostReturnInMemory` | 返回值是否走隐藏指针（内存化） |
| `hostReturnFloat` / `hostReturnSize` | 返回值在浮点寄存器还是整型寄存器、几个 DWORD |
| `paramSize` | 参数在 VM 栈上占的 DWORD 总数（调用后要弹掉多少） |
| `takesObjByVal` | 是否有类对象按值传参（调用期需走慢速路径逐个分类） |
| `baseOffset` | 多继承 this 调整量（见 §6） |
| `compositeOffset` / `isCompositeIndirect` | 组合模式（成员对象的方法注册给外层类）的偏移/间接 |
| `cleanArgs` | 调用后需要 release/destruct+free 的参数清单 |

**第一步：DetectCallingConvention（as_callfunc.cpp:56-177）**，在 RegisterGlobalFunction
（as_scriptengine.cpp:3100）、RegisterObjectMethod（:2935）等入口调用。它把公开枚举
`asECallConvTypes`（angelscript.h:199-210，asCALL_CDECL/STDCALL/THISCALL_ASGLOBAL/THISCALL/
CDECL_OBJLAST/CDECL_OBJFIRST/GENERIC/THISCALL_OBJLAST/THISCALL_OBJFIRST）映射为
internalCallConv，并顺带做防呆：`asSFuncPtr::flag`（1=asFUNCTION 包装的 generic、2=普通函数、
3=asMETHOD 包装的方法）与声称的约定不匹配则返回 `asWRONG_CALLING_CONV`（:65-70）。
对 THISCALL 它还要从成员函数指针里萃取信息（见 §6）。

**第二步：PrepareSystemFunction（as_callfunc.cpp:255-557）**，在引擎首次执行前的
`PrepareEngine()` 中对所有系统函数统一执行（as_scriptengine.cpp:3374-3384；generic 走精简版
PrepareSystemFunctionGeneric，as_callfunc.cpp:180-252，只算 paramSize 和 cleanArgs）。走读：

- **返回值分类**（:267-450）：引用/句柄按指针大小返回（:267-272）；注册类型按值返回时看
  类型标志——带 `COMPLEX_RETURN_MASK` 标志（MSVC x64 上 = 有构造/析构/赋值/拷贝构造，
  as_config.h:526）的一律内存化 `hostReturnInMemory = true`（:301-305）；简单类超过 2 个
  DWORD（64 位平台且有 128 位原语时为 4）也内存化（:308-318）；`asOBJ_APP_FLOAT` 标记
  `hostReturnFloat = true`（:406-411）；原生 float/double 靠与 ttFloat/ttDouble 比对判定
  （:437-443）。另有一批平台修正宏（THISCALL/CDECL/STDCALL_RETURN_SIMPLE_IN_MEMORY，
  :347-379）处理"某些编译器无论多小都内存化"的怪癖。
- **paramSize**（:453）：直接取 `func->GetSpaceNeededForArguments()`。
- **按值传对象的合法性检查**（:457-500）：类型必须带 asOBJ_APP_CLASS/PRIMITIVE/FLOAT/ARRAY
  标志之一，否则报配置错误——这就是你注册 `void f(MyStruct)` 忘了给 MyStruct 正确 flags 时
  看到的 "Can't pass type by value" 的出处。
- **cleanArgs**（:502-554）：预先算好哪些参数调用后需善后。MSVC x64 定义了
  `AS_CALLEE_DESTROY_OBJ_BY_VAL`（as_config.h:523），按值传入的对象由被调方析构，宿主只
  free；而大对象/复杂对象实际以指针传递（`AS_LARGE_OBJS_PASSED_BY_REF`，最小 3 DWORD，
  as_config.h:524-525），内存归调用方清理。

## 3. 调用期：CallSystemFunction（公共前置） → CallSystemFunctionNative（平台）

VM 执行到 `asBC_CALLSYS` 时（as_context.cpp:3008-3019）调 `CallSystemFunction(id, context)`
（as_callfunc.cpp:627-976），返回值是"要从 VM 栈弹掉多少 DWORD"。公共前置做四件事：

1. generic 约定直接分流到 `context->CallGeneric()`（:634-635，见 §7）。
2. **取对象指针**（:654-757）：THISCALL 系约定从 VM 栈头部弹出 obj（`args` 前移
   AS_PTR_SIZE），null 检查后施加组合偏移（:700-701）与多继承 baseOffset（:711）。
   THISCALL_ASGLOBAL 则直接用注册时存的 `auxiliary`（:690-695）。
3. **取隐藏返回指针**（:759-769）：`descr->DoesReturnOnStack()` 为真时，VM 编译器已在栈上
   压了一个指向预分配返回缓冲的指针，这里取出作为 `retPointer`。
4. 用 try/catch 包住平台调用（:790-805），宿主函数抛 C++ 异常会被转成脚本异常。

调用后按 `hostReturnSize`/`hostReturnFloat` 把 `retQW[2]` 写回 value register、object
register 或 retPointer（:810-932），最后按 cleanArgs 释放参数（:934-973）。

### Windows x64 实现（as_callfunc_x64_msvc.cpp + .asm）

C++ 侧 `CallSystemFunctionNative`（as_callfunc_x64_msvc.cpp:54-210）先在本地
`asQWORD allArgBuffer[64]` 和 `asQWORD floatArgBuffer[4]`（:64-65）里**线性摆好最终参数序列**：

- THISCALL 系：obj 作第 0 个参数（:70-75）。
- `hostReturnInMemory`：**`callConv++`**（:80，利用枚举 +1 = RETURNINMEM 的布局技巧），并把
  retPointer 插为下一个参数（:83）——这正是 MS x64 ABI"大于 8 字节的返回值由调用方传隐藏
  缓冲指针（放 RCX，若有 this 则 this 在 RCX、缓冲在 RDX）"的软件实现。
- 逐参数搬运（:116-181）：对象按值时，大于等于 3 DWORD 或带 COMPLEX_MASK 的**改传指针**
  （:121-127，MS ABI 规定超 8 字节的结构体按引用传）；小对象把内容 memcpy 进参数槽后
  `CallFree` 掉 VM 分配的临时内存（:129-140）。float/double 除了放进 allArgBuffer，若序号
  在前 4 个之内还要**复制一份**到 floatArgBuffer 的同下标处（:160-161、172-173）——因为
  MS x64 中第 N 个参数如果是浮点就走 XMM(N)，整型就走第 N 个整型寄存器，位置一一对应。
- 虚方法：从对象头部取 vtable，`func = vftable[asPWORD(func)>>2]`（:108-110，MSVC 的成员
  函数指针对虚函数存的是 thunk 地址，AngelScript 用的是萃取出的字节偏移，除 4 得索引）。

然后进入手写汇编 `CallX64(args, floatArgs, paramSize, func)`（.asm:36-156），对照标签读：

```asm
CallX64 PROC FRAME
    push rbp ... push rbx        ; :43-60 保存 9 个非易失寄存器
    sub rsp, 050h                ; :61 本地帧
    mov r14, r9                  ; :68 第4个参数 func 存入 r14
    mov rdi, r8                  ; :74 rdi = paramSize
    add rdi, 32                  ; :75 ← 影子空间：即使不足4参也要留 32 字节
    ...and rsi, 8h...            ; :80-84 凑 16 字节对齐后 sub rsp, rdi
    mov rcx, [rsi]               ; :98  第1参 → RCX
    mov rdx, [rsi+8]             ; :99  第2参 → RDX
    mov r8,  [rsi+16]            ; :100 第3参 → R8
    mov r9,  [rsi+24]            ; :101 第4参 → R9  （无脑装载，多装无害）
    sub r12d, 32 / js copyfloat  ; :104-106 参数 ≤4 个则跳过栈复制
copyoverflow:                    ; :115-121 第5个起逐 QWORD 拷到 rsp+32 之后
    mov r15, [rsi] / mov [r13], r15 / ...
copyfloat:                       ; :123-131 floatArgs 非空则装载
    movlpd xmm0..xmm3, [r11+0..24]   ; 前4参的浮点副本 → XMM0-3
callfunc:
    call r14                     ; :136 真正的调用
    mov rsp, rbp / pop ...       ; :139-151 恢复
    ret                          ; :154 整型/指针返回值已在 RAX
```

浮点返回值不经 RAX，C++ 侧调用后立即用 `GetReturnedFloat()`/`GetReturnedDouble()`
（.asm:163-206，`movss/movlpd [rsp], xmm0` 再读回 eax/rax）把 XMM0 捞出来
（as_callfunc_x64_msvc.cpp:201-207）。**这依赖"call 之间 XMM0 未被破坏"，是这套实现里最
微妙的约定之一。**

## 4. 两大 x64 ABI 对比（MSVC vs System V）

as_callfunc_x64_gcc.cpp 实现 System V（Linux/macOS/BSD）。同一颗 CPU，规则大不相同：

| | Windows x64 (MSVC) | System V AMD64 (gcc/clang) |
|---|---|---|
| 整型寄存器 | RCX, RDX, R8, R9（4 个） | RDI, RSI, RDX, RCX, R8, R9（6 个，gcc.cpp:60、:123-128） |
| 浮点寄存器 | XMM0-3（4 个） | XMM0-7（8 个，:61、:130-137） |
| 位置分配 | 整型/浮点**共享位置计数**（第 N 参占用第 N 槽） | 整型、浮点**各自独立计数** |
| 影子空间 | 调用方必须预留 32 字节（.asm:75） | 无；但有 128 字节 red zone 需跳过（gcc.cpp:96） |
| 结构体按值 | >8 字节一律退化为指针 | ≤16 字节可拆进寄存器，按成员类型分配整型/浮点寄存器（`SPLIT_OBJS_BY_MEMBER_TYPES`） |
| 返回值 | RAX 或 XMM0；>8 字节走隐藏指针 | RAX:RDX 或 XMM0:XMM1（gcc.cpp:151-163）；>16 字节走隐藏指针 |

实现策略也不同：gcc 版先把每个参数分类为 `x64INTARG`/`x64FLOATARG`（:311-408），再重排进
`tempBuff`——前 6 槽整型寄存器、接着 8 槽 XMM、其余**倒序**放栈参区（:443-476，倒序是为了
汇编里能直接 `pushq`，:113-119 的 Lloopstack 从偏移 112=(6+8)*8 处开始取）。正因结构体会按
成员类型拆寄存器，System V 上按值传/返回类对象必须注册 `asOBJ_APP_CLASS_ALLINTS` 或
`ALLFLOATS` 标志，否则 PrepareSystemFunction 直接报错（as_callfunc.cpp:382-398、:476-497）。

## 5. 特殊情形

- **类对象按值何时退化为隐藏指针/引用**：返回值——带 COMPLEX_RETURN_MASK（有非平凡构造/
  析构/拷贝）或超过寄存器容量（MSVC >8 字节，SysV >16 字节）时 `hostReturnInMemory=true`，
  调用方传缓冲指针；参数——MSVC x64 上 ≥3 DWORD 或带 COMPLEX_MASK 时传指针
  （as_callfunc_x64_msvc.cpp:121-127），且由被调方析构（AS_CALLEE_DESTROY_OBJ_BY_VAL）。
- **thiscall 的 this 调整**：多继承下 `&Derived::MethodOfSecondBase` 的成员函数指针里带一个
  delta，注册期用 `MULTI_BASE_OFFSET(ptr)` 萃取存入 `baseOffset`（as_callfunc.cpp:149），
  调用期加到对象指针上（:711）。虚继承（HAVE_VIRTUAL_BASE_OFFSET 非零）不支持，直接
  asNOT_SUPPORTED（:159-163）。
- **虚函数指针萃取**：Itanium ABI（gcc/clang）用函数指针的最低位标记虚函数，指针值实为
  "vtable 字节偏移+1"——DetectCallingConvention 检测到奇数就把约定升级为
  ICC_VIRTUAL_THISCALL（:145-148；ARM/MIPS 上该标志位挪到了 delta 里，:150-157）。调用期
  从对象取 vtable 再索引：gcc 版 `func = vftable[FuncPtrToUInt(func) >> 3]`
  （as_callfunc_x64_gcc.cpp:210-211），MSVC 版 `>>2`（msvc.cpp:108-110）。
- **成员函数指针怎么塞进 asSFuncPtr**：`asMETHOD` 宏（angelscript.h:504-505）经
  `asSMethodPtr<sizeof(方法指针)>::Convert()` 按**指针大小**分发到不同特化
  （angelscript.h:1301-1415）——MSVC 的方法指针随单继承/多继承/虚继承从 8 字节膨胀到
  16/24 字节，模板靠 sizeof 分辨并如实拷贝（虚继承的特化会做手脚让引擎能检测并拒绝，
  :1381）。MULTI_BASE_OFFSET 等宏按编译器各定义一版（as_config.h:297-310 有说明，
  :404、:1353 等处定义），细节多而杂，知道"它从方法指针的第二个机器字里挖 delta"即可。

## 6. generic 约定：永远可用的兜底

`asCALL_GENERIC` 不需要任何 ABI 知识：宿主函数签名统一为 `void f(asIScriptGeneric*)`，
参数搬运的责任转嫁给了**函数作者本人**。`AS_MAX_PORTABILITY` 定义时它是唯一可用的约定
（此时 CallSystemFunction 只认 generic，as_callfunc.cpp:561-573；RegisterGlobalFunction
直接拒绝其它约定，as_scriptengine.cpp:3094-3097）。

实现本质一句话：**asCGeneric 只是 VM 栈指针的一层薄封装**（as_generic.h:97-103——成员就是
engine、sysFunction、currentObject、`asDWORD *stackPointer`、objectRegister、returnVal）。
`GetArgDWord(n)`（as_generic.cpp:150-170）就是累加前 n 个参数的 GetSizeOnStackDWords() 得出
offset，然后 `*(asDWORD*)&stackPointer[offset]`——直接读 VM 栈原地的字节；
`SetReturnDWord`（:364-377）写入成员 `returnVal`，`CallGeneric`（as_context.cpp:5876-5999）
在调用结束后把它拷进 `m_regs.valueRegister`（:5956）。按值返回对象时
`GetAddressOfReturnLocation()`（as_generic.cpp:508-527）返回栈上 args 前面的预分配缓冲。

性能上定性说：generic 调用没有寄存器装配和汇编跳转，但多了 asCGeneric 对象构造、每次取参的
O(参数序号) 偏移累加、以及宿主侧一层间接调用——对"参数少、函数体大"的函数差异可忽略；对
数学函数这类微函数，开销约为原生 cdecl 的 1.5~2 倍量级，仍远小于脚本解释本身。想要 generic
的可移植性又不想手写搬运代码，可用 add_on/autowrapper/aswrappedcall.h 的模板自动生成
generic 包装（`asFUNCTION(f)` → `WRAP_FN(f)` 一行替换）。

## 7. 关键代码走读

1. **约定枚举的 +1 技巧**：as_callfunc.h:70-96 每个 ICC_XXX 后紧跟 ICC_XXX_RETURNINMEM；
   as_callfunc_x64_msvc.cpp:77-84 `if(sysFunc->hostReturnInMemory) { callConv++; ... }`
   ——一次自增完成"切换到隐藏指针版约定"。
2. **防呆检查**：as_callfunc.cpp:63-71，`asSFuncPtr.flag`（asFUNCTION 置 2、asMETHOD 置 3、
   generic 包装置 1）与用户声称的 callConv 交叉验证——注册期就拦住"拿方法指针冒充全局函数"。
3. **影子空间与对齐**：as_callfunc_x64_msvc_asm.asm:74-84，`add rdi,32` 留影子空间，
   `and rsi,8h` 系列凑 16 字节栈对齐——两条 MS x64 ABI 硬规矩的直接体现。
4. **浮点参数的双写**：as_callfunc_x64_msvc.cpp:160-161 + .asm:128-131——C++ 侧把前 4 个
   位置上的 double 同时写进 floatArgBuffer，汇编侧无条件 `movlpd xmm0..3`。多装载不匹配
   类型的寄存器是无害的，省去了逐参数判断。
5. **SysV 的参数重排注释**：as_callfunc_x64_gcc.cpp:424-442 作者自问自答 "Q: WTF is going
   on here!?"——tempBuff 前 6 槽整型、中 8 槽浮点、尾部倒序栈参，是为了让内联汇编
   X64_CallFunction（:67-170）能用固定指令序列装载。
6. **generic 的"零魔法"**：as_generic.cpp:242-259 GetArgAddress 全部内容 = 类型检查 + 偏移
   累加 + `(void*)*(asPWORD*)&stackPointer[offset]`。

## 8. 如果你要自己实现

**强烈建议第一版只做 generic 约定**：定义你自己的 asIScriptGeneric 等价物，包一层 VM 栈指针
即可——零汇编、全平台可移植，AngelScript 自己也把它当作 AS_MAX_PORTABILITY 下的唯一路径。
配合 autowrapper 式的 C++ 模板生成包装，易用性上并不输原生约定。

要做原生约定时的开发顺序：

1. 先只支持一个平台（如 x64 SysV），只支持 cdecl + 整型参数 ≤6 个，返回值 ≤8 字节。
2. 加浮点参数（独立计数）→ 加栈溢出参数 → 加隐藏返回指针 → 最后才碰按值传结构体
   （这是错误率最高的部分，寄存器分类规则极繁琐）。
3. thiscall/虚函数留到最后，且接受"虚继承不支持"这类合理裁剪（AngelScript 也是这么做的）。
4. **测试方法**：照抄 sdk/tests/test_feature 的 calling convention 测试组——
   testcdecl_class*.cpp（1/2/3 个 float 成员、构造析构组合的按值返回）、teststdcall4args.cpp、
   test_thiscall_class.cpp / test_thiscall_asglobal.cpp / test_thiscall_as_method.cpp、
   test_cdecl_objlast.cpp、testnotcomplexthiscall.cpp / testnotcomplexstdcall.cpp。这批用例
   系统性覆盖了"大小 × 成员类型 × 复杂度 × 约定"的组合矩阵，每个都是历史上真实翻过车的坑。
   每加一种情形就在两种优化级别（-O0/-O2）下跑一遍——ABI bug 常常只在优化后现形。

## 9. 动手实验

1. **单步看寄存器装配**：注册 `struct Vec2 { float x, y; }; float len(Vec2 v)`（记得
   `asOBJ_VALUE | asOBJ_APP_CLASS_ALLFLOATS | asOBJ_APP_CLASS_ALIGN8` 类标志），在
   as_callfunc_x64_msvc.cpp:198 的 `CallX64(...)` 下断点，切到反汇编窗口跟进
   as_callfunc_x64_msvc_asm.asm——观察 8 字节的 Vec2 整个作为一个 QWORD 进了 RCX（MSVC 不拆
   结构体）；再把成员改成 3 个 float，观察它变成 ≥3 DWORD 后退化为指针传递（:121-127 分支）。
   对照在 Linux 上跑同样实验，Vec2 会进 XMM0（ALLFLOATS 拆分，as_callfunc_x64_gcc.cpp:386-405）。
2. **量化 generic vs cdecl**：sdk/tests/test_performance 工程里挑 test_call.cpp（脚本反复调
   宿主函数的场景），把被测函数分别用 asCALL_CDECL 与 asCALL_GENERIC（配 aswrappedcall.h 的
   WRAP_FN）注册各跑一次，对比耗时。再把函数体从空函数换成有实际工作量的，观察差距被稀释。

## 10. 扩展阅读

- Microsoft x64 calling convention（影子空间、RCX/RDX/R8/R9、结构体传参规则的官方定义）：
  https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention
- System V AMD64 ABI：维基百科 "x86 calling conventions" 条目（含两套 ABI 对照表），及
  正式规范 System V Application Binary Interface AMD64 Architecture Processor Supplement。
- as_callfunc.cpp:48-54 引用的两篇文献：CodeProject《Member Function Pointers and the
  Fastest Possible C++ Delegates》（各编译器方法指针内存布局），以及 LLVM
  ItaniumCXXABI::EmitLoadOfMemberFunctionPointer 的源码注释（Itanium/ARM64 方法指针结构）。

---

**一句话总结**：as_callfunc 在运行时手工复刻 C++ 编译器的调用约定——注册期由
DetectCallingConvention + PrepareSystemFunction 把函数签名预计算成 asSSystemFunctionInterface
（参数尺寸、对象指针位置、返回值是否内存化），调用期由公共层 CallSystemFunction 处理 this
调整与隐藏返回指针，再交给各平台的 CallSystemFunctionNative 把 VM 栈字节装配进寄存器与 C++
栈（Windows x64 上即 RCX/RDX/R8/R9/XMM0-3 + 影子空间的手写汇编 CallX64）；而 asCGeneric 约定
让宿主函数直接读写 VM 栈，以少量性能换来零汇编的全平台可移植性。
