// comsig.h — COM 事件接收器的回调参数类型码
//
// 原始（非 IDispatch）接口的方法签名来自类型库（FUNCDESC / ELEMDESC），
// 参数类型比通用 FFI 的 int/pointer 更丰富：BSTR、VARIANT、接口指针。
// 这里定义 COM 专用的参数类型码（ApiSig.params 里 > 12 的部分），
// 转换逻辑由 com.cpp 实现（COM 宿主侧），ffi.cpp 的回调跳板按类型码分派。
//
// 单独成文件是为了避开 engine/common.h —— 那份头文件由并发的 IDE 会话
// 同时改动，不适合承载 COM 专有的常量。
#pragma once

#include "common.h"

namespace NodioVM
{
    enum ComParamCode
    {
        kComBstr     = 13,   // BSTR        -> aardio 字符串
        kComDispatch = 14,   // IDispatch*  -> COM 对象
        kComUnknown  = 15,   // IUnknown*   -> IUnknown 指针（借用）
        kComVariant  = 16,   // VARIANT*    -> aardio 值
        kComIface    = 17,   // 接口指针    -> COM 对象 / IUnknown 指针
    };

    // com.cpp 实现，ffi.cpp 的回调跳板调用
    Value ComArgFromBstr(void* p);
    Value ComArgFromDispatch(void* p);
    Value ComArgFromUnknown(void* p);
    Value ComArgFromVariant(void* p);
    Value ComArgFromIface(void* p);

    // 调试追踪（写文件，避免控制台重定向吞掉输出），com.cpp 实现
    void ComRawTrace(const wchar_t* fmt, ...);
}
