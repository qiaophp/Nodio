/*
可遵循 aardio 用户协议与 aardio 开源许可证在 aardio 程序中自由使用本组件以及本组件源码,
禁止在非 aardio 开发的程序中引用本组件的任何部份(包含但不限于本组件源码、使用此源码生成的 DLL )
*/
using System;
using System.Reflection;
using System.Collections;
using System.Runtime.InteropServices;

namespace aardio.Interop
{
    public class DispatchableDelegate
    {
        private object invokeableObj;
        public DispatchableDelegate(object dispObject) { invokeableObj = dispObject; }

        private bool isComVisible(Type type)
        {
            bool comVisible = true;
            //first check if the type has ComVisible defined for itself
            var typeAttributes = type.GetCustomAttributes(typeof(ComVisibleAttribute), true);
            if (typeAttributes.Length > 0)
            {
                comVisible = ((ComVisibleAttribute)typeAttributes[0]).Value;
            }
            else
            {
                //no specific ComVisible attribute defined, return the default for the assembly
                var assemblyAttributes = type.Assembly.GetCustomAttributes(typeof(ComVisibleAttribute), true);
                if (assemblyAttributes.Length > 0)
                {
                    comVisible = ((ComVisibleAttribute)assemblyAttributes[0]).Value;
                }
            }
            return comVisible;
        }


        static Type ColorType = typeof(System.Drawing.Color);
        static Type ComVisibleAttributeType = typeof(ComVisibleAttribute);
        [ComVisible(true)]
        public object Invoke(params object[] args) {

            if (args != null) {
                object arg;
                Type t;
 
                for (int i = 0; i < args.Length; i++)
                {
                    arg = args[i];
                    if (arg != null)
                    {
                        t = arg.GetType();
                        if ( t.IsPrimitive || t.IsEnum ) continue;
                        if (typeof(string) == t) continue;
 
                        if (t == ColorType)
                        {
                            args[i] = ((System.Drawing.Color)arg).ToArgb();
                            continue;
                        }

                        //if ( !isComVisible(t) ) args[i] = null;


                        if (!t.IsValueType)
                        {
                            if (t.IsArray)
                            {
                                Type tEle = t.GetElementType();
                                Array arr = arg as Array;

                                if (arr.GetLength(0) == 0) args[i] = new aardio.Interop.DispatchableObject(arg, false);
                                else if(typeof(string) == tEle) continue;
                                else if(tEle.IsArray)
                                {
                                    object first = aardio.Interop.Utility.WrapNonPrimitiveValueToAnyObjectRef(arr.GetValue(0));
                                    if ((first != null) && (first.GetType() == aardio.Interop.Utility.DispatchableObjectType))
                                    {
                                        args[i] = new aardio.Interop.DispatchableObject(arg, false);
                                    }
                                }
                                else if (!(tEle.IsPrimitive || tEle.IsEnum))
                                {
                                    args[i] = Utility.WrapNonPrimitiveValueToAnyObjectRef(arg);
                                }
                            }
                            else if (t.IsClass)
                            {
                                try
                                {
                                    var ptr = Marshal.GetIDispatchForObject(arg);
                                    if (ptr != null) Marshal.Release(ptr);
                                }
                                catch (InvalidOperationException)
                                {
                                    args[i] = new aardio.Interop.DispatchableObject(arg, false);
                                }
                                catch (Exception)
                                {

                                }
                            }
                        }

                    }
                }
         
            }
             
            return invokeableObj.GetType().InvokeMember("", BindingFlags.InvokeMethod, null, invokeableObj, args);
        }

        public TResult Func<TResult>() { return (TResult)Invoke(); }
        public TResult Func<T, TResult>(T arg) { return (TResult)Invoke(arg); }
        public TResult Func<T1, T2, TResult>(T1 arg1, T2 arg2) { return (TResult)Invoke(arg1, arg2); }
        public TResult Func<T1, T2, T3, TResult>(T1 arg1, T2 arg2, T3 arg3) { return (TResult)Invoke(arg1, arg2, arg3); }
        public TResult Func<T1, T2, T3, T4, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4) { return (TResult)Invoke(arg1, arg2, arg3, arg4); }
        public TResult Func<T1, T2, T3, T4, T5, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5); }
        public TResult Func<T1, T2, T3, T4, T5, T6, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5, arg6); }
        public TResult Func<T1, T2, T3, T4, T5, T6, T7, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7); }
        public TResult Func<T1, T2, T3, T4, T5, T6, T7, T8, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8); }
        public TResult Func<T1, T2, T3, T4, T5, T6, T7, T8, T9, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9); }
        public TResult Func<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9, T10 arg10) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10); }
        public TResult Func<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9, T10 arg10, T11 arg11) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11); }
        public TResult Func<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, TResult>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9, T10 arg10, T11 arg11, T12 arg12) { return (TResult)Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12); }

        public void Action() { Invoke(); }
        public void Action<T>(T arg) { Invoke(arg); }
        public void Action<T1, T2>(T1 arg1, T2 arg2) { Invoke(arg1, arg2); }
        public void Action<T1, T2, T3>(T1 arg1, T2 arg2, T3 arg3) { Invoke(arg1, arg2, arg3); }
        public void Action<T1, T2, T3, T4>(T1 arg1, T2 arg2, T3 arg3, T4 arg4) { Invoke(arg1, arg2, arg3, arg4); }
        public void Action<T1, T2, T3, T4, T5>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5) { Invoke(arg1, arg2, arg3, arg4, arg5); }
        public void Action<T1, T2, T3, T4, T5, T6>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6) { Invoke(arg1, arg2, arg3, arg4, arg5, arg6); }
        public void Action<T1, T2, T3, T4, T5, T6, T7>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7) { Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7); }
        public void Action<T1, T2, T3, T4, T5, T6, T7, T8>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8) { Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8); }
        public void Action<T1, T2, T3, T4, T5, T6, T7, T8, T9>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9) { Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9); }
        public void Action<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9, T10 arg10) { Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10); }
        public void Action<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9, T10 arg10, T11 arg11) { Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11); }
        public void Action<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12>(T1 arg1, T2 arg2, T3 arg3, T4 arg4, T5 arg5, T6 arg6, T7 arg7, T8 arg8, T9 arg9, T10 arg10, T11 arg11, T12 arg12) { Invoke(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12); }


        public Delegate CreateDelegate(Type delegateType)
        {
            var delegateMethod = delegateType.GetMethod("Invoke");
            var delegateReturn = delegateMethod.ReturnType;
            var delegateParameters = delegateMethod.GetParameters();
            var delegateAction = delegateReturn == typeof(void);

            Type objectType = this.GetType();
            var methods = objectType.GetMethods();

            MethodInfo method = null;
            ParameterInfo[] methodParameters = null;
            Type methodReturn = null;

            var delegateMethodName = delegateAction ? "Action" : "Func";
            foreach (var methodInfo in methods)
            {
                if (methodInfo.Name != delegateMethodName)
                {
                    continue;
                }
                methodParameters = methodInfo.GetParameters();
                methodReturn = methodInfo.ReturnType;

                if (methodParameters.Length != delegateParameters.Length)
                {
                    continue;
                }

                method = methodInfo;
                break;
            }

            if (method == null)
            {
                throw new Exception("DispatchableDelegate: Method not found");
            }

            if (method.IsGenericMethodDefinition)
            {
                var genericArguments = method.GetGenericArguments();
                var genericParameters = new Type[genericArguments.Length];

                int genericArgumentIndex;
                if (!delegateAction) { 
                    genericArgumentIndex = Array.IndexOf(genericArguments, methodReturn);
                    if (genericArgumentIndex != -1)
                    {
                        genericParameters[genericArgumentIndex] = delegateReturn;
                    }
                }

                for (int i = 0; i < methodParameters.Length; ++i)
                {
                    var methodParameter = methodParameters[i];
                    genericArgumentIndex = Array.IndexOf(genericArguments, methodParameter.ParameterType);

                    if (genericArgumentIndex == -1) continue;
                    genericParameters[genericArgumentIndex] = delegateParameters[i].ParameterType;
                }

                if (Array.IndexOf(genericParameters, null) != -1)
                {
                    throw new Exception("DispatchableDelegate: Failed to resolve some generic parameters.");
                }

                var concreteMethod = method.MakeGenericMethod(genericParameters);


                return Delegate.CreateDelegate(delegateType, this, concreteMethod);
            }
            else
            {
                return Delegate.CreateDelegate(delegateType, this, method);
            }
        }

        public static object CreateDelegateByMemberName(object target ,string k, object func)
        {
            if (typeof(Delegate).IsAssignableFrom(func.GetType().BaseType)) return func; 
            Type tAny = target.GetType();
            
            FieldInfo fieldInfo = tAny.GetField(k, BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.FlattenHierarchy | BindingFlags.GetProperty | BindingFlags.GetField);
            if (fieldInfo!=null)
            {
                return new DispatchableDelegate(func).CreateDelegate(fieldInfo.FieldType); 
            }
            else
            {
                PropertyInfo propInfo = tAny.GetProperty(k, BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.FlattenHierarchy | BindingFlags.GetProperty | BindingFlags.GetField);
                if (propInfo != null)
                {
                    return new DispatchableDelegate(func).CreateDelegate(propInfo.PropertyType); 
                }
            }

            EventInfo eventInfo = tAny.GetEvent(k);
            if (eventInfo != null)
            {
                return new DispatchableDelegate(func).CreateDelegate(eventInfo.EventHandlerType);
            }

            return null;
        }
        

        public static object CombineDelegateByMemberName(object target, string k, object func)
        {
            bool funcIsDelegate = typeof(Delegate).IsAssignableFrom(func.GetType().BaseType);
            Type tAny = target.GetType();

            FieldInfo fieldInfo = tAny.GetField(k, BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.FlattenHierarchy | BindingFlags.GetProperty | BindingFlags.GetField);
            if (fieldInfo != null)
            {
                if(!funcIsDelegate) func = new DispatchableDelegate(func).CreateDelegate(fieldInfo.FieldType); 
                object newfunc = Delegate.Combine((Delegate)fieldInfo.GetValue(target), (Delegate)func);
                fieldInfo.SetValue(target, newfunc);
            }
            else
            {
                PropertyInfo propInfo = tAny.GetProperty(k, BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.FlattenHierarchy | BindingFlags.GetProperty | BindingFlags.GetField);
                if (propInfo != null)
                {
                    if (!funcIsDelegate) func = new DispatchableDelegate(func).CreateDelegate(propInfo.PropertyType);
                    object newfunc = Delegate.Combine((Delegate)fieldInfo.GetValue(target), (Delegate)func);
                    propInfo.SetValue(target, newfunc,null);
                }
            }

            EventInfo eventInfo = tAny.GetEvent(k);
            if (eventInfo != null)
            {
                if (!funcIsDelegate) func = new DispatchableDelegate(func).CreateDelegate(eventInfo.EventHandlerType);
                eventInfo.AddEventHandler(target, (Delegate)func); 
            }

            return func;
        }

        public static bool RemoveDelegateByMemberName(object target, string k, object func)
        {
            if (!typeof(Delegate).IsAssignableFrom(func.GetType().BaseType)) return false;

            Type tAny = target.GetType();

            FieldInfo fieldInfo = tAny.GetField(k, BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.FlattenHierarchy | BindingFlags.GetProperty | BindingFlags.GetField);
            if (fieldInfo != null)
            { 
                object newfunc = Delegate.RemoveAll((Delegate)fieldInfo.GetValue(target), (Delegate)func);
                fieldInfo.SetValue(target, newfunc);
                return true;
            }
            else
            {
                PropertyInfo propInfo = tAny.GetProperty(k, BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.FlattenHierarchy | BindingFlags.GetProperty | BindingFlags.GetField);
                if (propInfo != null)
                { 
                    object newfunc = Delegate.RemoveAll((Delegate)fieldInfo.GetValue(target), (Delegate)func);
                    propInfo.SetValue(target, newfunc, null);
                    return true;
                }
            }

            EventInfo eventInfo = tAny.GetEvent(k);
            if (eventInfo != null)
            { 
                eventInfo.RemoveEventHandler(target, (Delegate)func);
                return true;
            }

            return false;
        }


        //参数不能为 null , 只能是委托或 aardio 对象,这里不作判断
        public static  object CombineDelegates(object a, object b)
        {
            Type ta = a.GetType();
            Type tb = b.GetType();
            bool da = typeof(Delegate).IsAssignableFrom(ta.BaseType);
            bool db = typeof(Delegate).IsAssignableFrom(tb.BaseType);

            if (da && tb.IsCOMObject)
            {
                var disp = new DispatchableDelegate(b);
                return Delegate.Combine((Delegate)a, (Delegate)disp.CreateDelegate(ta));
            }
            else if (db && ta.IsCOMObject)
            {
                var disp = new DispatchableDelegate(a);
                return Delegate.Combine((Delegate)disp.CreateDelegate(tb), (Delegate)b);
            }
            else if (da && db)
            {
                return Delegate.Combine((Delegate)a, (Delegate)b);
            }
            else
            {
                throw new Exception("Failed to combine delegates");
            }
        }

        public static object RemoveDelegate(object a, object b)
        {
            Type ta = a.GetType();
            Type tb = b.GetType();
            bool da = typeof(Delegate).IsAssignableFrom(ta.BaseType);
            bool db = typeof(Delegate).IsAssignableFrom(tb.BaseType);

            if (da && db)
            { 
                return Delegate.RemoveAll((Delegate)a, (Delegate)b);
            } 
            else
            {
                throw new Exception("Failed to remove delegate");
            }
        }
    }
}
