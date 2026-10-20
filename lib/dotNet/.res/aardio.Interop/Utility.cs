/*
可遵循 aardio 用户协议与 aardio 开源许可证在 aardio 程序中自由使用本组件以及本组件源码,
禁止在非 aardio 开发的程序中引用本组件的任何部份(包含但不限于本组件源码、使用此源码生成的 DLL )
*/
using System;
using System.CodeDom.Compiler;
using System.Collections;
using System.Reflection;
using System.Runtime.InteropServices;
using System.IO;
using System.Collections.Generic;

namespace aardio.Interop
{
    [Guid("7C856F49-0310-40F6-A1F2-B7BBB4C48F30")] //不可修改 GUID
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [ComImport]
    public interface IDispatchableObject
    {
    }

    [ClassInterface(ClassInterfaceType.AutoDispatch), ComVisible(true)]
    public class DispatchableObject: IDispatchableObject, IEnumerable
    {
        private object underlyingObject;
        private Type type;
        public DispatchableObject(object obj, bool byRef) {
            type = obj.GetType();

            if (type == Utility.DispatchableObjectType)
            {
                obj = (obj as DispatchableObject).Value;
                type = obj.GetType();
            }

            underlyingObject = obj; 
            type = underlyingObject.GetType(); 
            ByRef = byRef; 
        }
        public bool ByRef; 
        
        [DispId(0)]
        public object this[params object[] args]
        {
            get
            {
                return type.InvokeMember("Item", BindingFlags.GetProperty | BindingFlags.Instance | BindingFlags.Public, null, underlyingObject, args);
               //if (underlyingObject is Array arr) return arr.GetValue(index);
               //return null;
            }
            set
            {
                type.InvokeMember("Item", BindingFlags.SetProperty | BindingFlags.Instance | BindingFlags.Public, null, underlyingObject, args);
                //if (underlyingObject is Array arr) arr.SetValue(value,index); 
            }
        } 

        public int Length
        {
            get {
                if (underlyingObject is Array arr) return arr.Length;
                return 0;
            } 
        }

        public Object Value
        {
            get {  return underlyingObject; }
            set { underlyingObject = value; }
        }

       
        public object Invoke(params object[] args) {
            if( typeof(Delegate).IsAssignableFrom(type))
            {
                return type.InvokeMember("", BindingFlags.InvokeMethod, null, underlyingObject, args);
            }
            
            return underlyingObject;  
        }
        
        public override string ToString()
        {
            return underlyingObject.ToString();
        }
        
        public IEnumerator GetEnumerator()
        {
            if (underlyingObject is Array arr) return arr.GetEnumerator();
            if (underlyingObject is IEnumerable) return type.InvokeMember("GetEnumerator", BindingFlags.InvokeMethod | BindingFlags.Instance | BindingFlags.Public, null, underlyingObject, null) as IEnumerator;
            throw new NotImplementedException();
        }
    }

    //*注意 COM 类里传来的 .Net 对象也会变成 System.__ComObject,非 COM 类才会还原 .Net 对象*/
    [ClassInterface(ClassInterfaceType.AutoDispatch), ComVisible(true)]
    public class Utility
    {
        public static Type DispatchableObjectType = typeof(DispatchableObject);
        static Type ColorType = typeof(System.Drawing.Color);
        static Type IntPtrType = typeof(System.IntPtr);
        static Type UIntPtrType = typeof(System.UIntPtr);
        static Type CharType = typeof(System.Char);

        public static object WrapNonPrimitiveValueToAnyObjectRef(object ret)
        {
            if (ret == null) return ret;

            Type t = ret.GetType();
            if (t.IsPrimitive || t.IsEnum) return ret;

            if (!t.IsValueType)
            {
                if (t.IsArray) {
                    Type tEle = t.GetElementType();
                    Array arr = ret as Array;

                    if (arr.GetLength(0) == 0) return new aardio.Interop.DispatchableObject(ret, false);
                    if (tEle.IsPrimitive || tEle.IsEnum || (typeof(string) == tEle)) return ret;

                    if (tEle.IsArray)
                    {
                        if (t.GetArrayRank() == 1)
                        {
                            return new aardio.Interop.DispatchableObject(ret, false);
                        }

                        object first = WrapNonPrimitiveValueToAnyObjectRef(arr.GetValue(0));
                        if (first != null) {
                            if (first.GetType() == DispatchableObjectType)
                            {
                                return new aardio.Interop.DispatchableObject(ret, false);
                            }
                        }
                    }
                    else
                    {
                        //System.Drawing.Color 如果是数组不作转换
                        return new DispatchableObject(ret, false);
                    }
                }
                else if (t.IsClass)
                {
                    try
                    {
                        var ptr = Marshal.GetIDispatchForObject(ret);
                        if (ptr != null) Marshal.Release(ptr);
                    }
                    catch (InvalidOperationException)
                    {
                        return new aardio.Interop.DispatchableObject(ret, false);
                    }
                    catch (Exception)
                    {
                        return new aardio.Interop.DispatchableObject(ret, false);
                        //return ret;
                    }
                }


                return ret;
            }

            if (t == ColorType) return (uint)((System.Drawing.Color)ret).ToArgb();
            return new DispatchableObject(ret, false);
        }

        public object CreateAnyObject(object v, bool byRef = false)
        {
            return new DispatchableObject(v, byRef);
        }

        public object CreateCharObject(object v, bool byRef = false)
        {
            var argType = v.GetType();
            if (argType == DispatchableObjectType)
            {
                DispatchableObject dv = (v as DispatchableObject);
                v = dv.Value;
                argType = v.GetType();
            }

            var argTypeCode = Type.GetTypeCode(argType); 
             

            if (argType.IsArray)
            {
                var tEle = argType.GetElementType();
                var eleTypeCode = Type.GetTypeCode(tEle);

                if (eleTypeCode == TypeCode.Double) {
                    var srcArr = (v as double[]);
                    var dstArr = Array.CreateInstance(CharType, srcArr.Length);
                    for (int n = 0; n < srcArr.Length; n++)
                    {
                        dstArr.SetValue(Convert.ToChar(Convert.ToUInt16(srcArr[n])), n);
                    }

                    return new DispatchableObject(dstArr, byRef);
                }
                else {
                    var srcArr = (v as ushort[]);
                    var dstArr = Array.CreateInstance(CharType, srcArr.Length);
                    for (int n = 0; n < srcArr.Length; n++)
                    {
                        dstArr.SetValue(Convert.ToChar(srcArr[n]), n);
                    }

                    return new DispatchableObject(dstArr, byRef);
                }
            }
            else if(argTypeCode == TypeCode.String)
            {
                return new DispatchableObject((v as string).ToCharArray(), byRef);
            }

            return new DispatchableObject(Convert.ToChar(v), byRef);
        }

        public object CreateColorObject(object v, bool byRef = false)
        {
            var argType = v.GetType();
            if (argType == DispatchableObjectType)
            {
                DispatchableObject dv = (v as DispatchableObject);
                v = dv.Value;
                argType = v.GetType();
            }

            var argTypeCode = Type.GetTypeCode(argType);
            if (IsNumericType(argType))
            { 
                return new DispatchableObject(ConvertNumToColor(v, argTypeCode), byRef);
            }
            else if(argType == ColorType) return new DispatchableObject(v, byRef);
            else if(argType.IsArray)
            {
                if (argType.GetElementType() == ColorType)
                {
                    return new DispatchableObject(v, byRef);
                }

                var srcArr = (v as double[]);
                if(srcArr==null) return null;

                var dstArr = Array.CreateInstance(ColorType, srcArr.Length);
                for (int n = 0; n < srcArr.Length; n++){
                    dstArr.SetValue(ConvertNumToColor(srcArr[n], TypeCode.Double), n);
                }

                return new DispatchableObject(dstArr, byRef);
            }

            return null;
        }


        private void setOutValue(object[] outArgs, object[] invokeArgs2)
        {
            for (int i = 0; i < outArgs.Length; i++)
            {
                if (outArgs[i] != null)
                {
                    (outArgs[i] as DispatchableObject).Value = invokeArgs2[i];
                }
            }
        }

        public CCodeCompiler CreateCompiler(string provideType)
        {
            object obj = this.loadAssembly("System").CreateInstance(provideType);
            if (obj == null)
            {
                return null;
            }
            return new CCodeCompiler(obj as CodeDomProvider);
        }

        public object GetGenericTypeByName(object assembly, string typeName,object[] typeObjects)
        {
            Type t = null;

            int typeLength = typeObjects.Length;
            if (typeLength <= 0) return GetTypeByName(assembly, typeName);

            try
            {
                t = (assembly as Assembly).GetType(typeName, false);
                if (t == null)   t = Type.GetType(typeName, false); 

                if ( t!= null)
                {

                    Type[] types = new Type[typeLength];

                    for (int i = 0; i < typeLength; i++)
                    { 
                        if (typeObjects[i] is Type)
                        {
                            types[i] = typeObjects[i] as Type;
                        }
                        else if (typeObjects[i] is string)
                        {
                            types[i] = Type.GetType(typeObjects[i] as string, true);
                        }
                        else
                        {
                            types[i] = typeObjects[i].GetType(); 
                            if(types[i] == DispatchableObjectType)
                            {
                                DispatchableObject dispObj = (typeObjects[i] as DispatchableObject);
                                types[i] = types[i].GetType();
                            }
                        }
                    }

                    return t.MakeGenericType(types); 
                }
            } 
            catch (Exception e)
            {
                throw e;
            }
            return t;
        }

        public object GetTypeByName(object assembly, string typeName)
        {
            Type t = null;
            try { 
                t = (assembly as Assembly).GetType(typeName, false);
                if (t == null) t = Type.GetType(typeName, false);
            }
            catch (TypeLoadException)
            {

            }
            catch (Exception e)
            {
                throw e;
            }
            return t;
        }

        public object GetClassTypeByName(object assembly, string typeName)
        {
            Type t = (assembly as Assembly).GetType(typeName, false);
            if (t != null && t.IsClass) return t;
            return null;
        }

        public object CallByMethodInfo(object tAnyObject, MethodInfo method, int invokeAttr, object args, object target)
        { 
            {
                Type tAny = (tAnyObject == null)  ? target.GetType(): tAnyObject as Type;

                ArrayList argArrayList = (args as ArrayList);

                if ((tAny == DispatchableObjectType) && (target != null))
                {
                    DispatchableObject dispObj = (target as DispatchableObject);
                    target = dispObj.Value;
                    //tAny = target.GetType();
                }

                bool hasOutValues = false;
                object[] outArgs = new object[argArrayList.Count];
                Type[] argTypeArray = new Type[argArrayList.Count];
                for (int i = 0; i < argArrayList.Count; i++)
                {
                    argTypeArray[i] = argArrayList[i] != null ? argArrayList[i].GetType() : null;
                    if (argTypeArray[i] == DispatchableObjectType)
                    {
                        DispatchableObject v = (argArrayList[i] as DispatchableObject);
                        if (v.ByRef)
                        {
                            outArgs[i] = argArrayList[i];
                            hasOutValues = true;
                        }

                        argArrayList[i] = v.Value;
                    }
                }
                if (!hasOutValues) outArgs = null;
 


                MethodInfo[] ms = { method };
                bool failed = true;

                object[] invokeArgs2 = null;
                object ret = InvokeMemberBaseMethodInfo(method.Name, ms, argArrayList, ref failed, target, invokeAttr, ref invokeArgs2);
                if (!failed)
                {
                    ret = WrapNonPrimitiveValueToAnyObjectRef(ret);
                    if ((outArgs != null) && (invokeArgs2 != null)) setOutValue(outArgs, invokeArgs2);
                    return ret;
                }
            }

            return null;
        }

        public object GetGenericMethod(object tAnyObject, string methodName, int invokeAttr, object[] typeObjects,object target )
        {
            try
            {
                Type tAny = (tAnyObject == null) ? target.GetType() : tAnyObject as Type;

                if ((tAny == DispatchableObjectType) && (target != null))
                {
                    DispatchableObject dispObj = (target as DispatchableObject);
                    target = dispObj.Value;
                    tAny = target.GetType();
                }

                int typeLength = typeObjects.Length;
                Type[] argTypes = new Type[typeLength];

                for (int i = 0; i < typeLength; i++)
                {
                    if (typeObjects[i] is Type)
                    {
                        argTypes[i] = typeObjects[i] as Type;
                    }
                    else if (typeObjects[i] is string)
                    {
                        argTypes[i] = Type.GetType(typeObjects[i] as string, true);
                    }
                    else
                    {
                        argTypes[i] = typeObjects[i].GetType(); 
                        if(argTypes[i] == DispatchableObjectType)
                        {
                            DispatchableObject dispObj = (typeObjects[i] as DispatchableObject);
                            argTypes[i] = target.GetType();
                        }
                    }
                }

                MethodInfo[] ms = tAny.GetMethods((BindingFlags)invokeAttr | BindingFlags.IgnoreReturn);

                for (int i = 0; i < ms.Length; i++)
                {
                    if (ms[i] is MethodInfo method)
                    {
                        if (methodName != method.Name) continue;
                        if (!method.IsGenericMethod) continue;
                        var genericArguments = method.GetGenericArguments();
                        if (genericArguments.Length != typeLength) continue;

                        var concreteMethod = method.MakeGenericMethod(argTypes);

                        return concreteMethod;
                    }
                } 

            }
            catch (TargetInvocationException targetEx)
            {
                if (targetEx.InnerException != null)
                {
                    throw targetEx.InnerException;
                }
            }

            return null; 
        }




        public object InvokeMember2(object tAnyObject , object assemblyName, string typeName, string methodName, int invokeAttr, object args, object target)
        {
            try
            {
                Type tAny = tAnyObject as Type;
                Assembly assembly = assemblyName as Assembly;

                if (assembly == null)
                {
                    assembly = this.loadAssembly(assemblyName as string);
                }
                if (assembly != null)
                {
                    if (tAny == null) tAny = assembly.GetType(typeName);
                    if (((BindingFlags)invokeAttr & BindingFlags.CreateInstance) == BindingFlags.CreateInstance)
                    {

                        if (tAny != null && tAny.IsClass)
                        {
                            return CreateInstanceByClassType(tAny, (args as ArrayList), target);
                        }

                    }

                    if( (invokeAttr & (16 | 8 | 256)) == (16 | 8 | 256))
                    {
                        if( (target == null) && (typeName !=null) && (methodName != null) )
                        {
                            Type tAny2 = assembly.GetType(typeName + "." + methodName);
                            if (tAny2 != null )
                            {
                                if(tAny2.IsClass) return CreateInstanceByClassType(tAny2, (args as ArrayList), target);
                                if (tAny2.IsValueType && !tAny2.IsEnum && !tAny2.IsPrimitive)
                                {
                                    return CreateInstanceByClassType(tAny2, (args as ArrayList), target);
                                }
                            }
                        }

                        if (tAny == null) throw new MissingMethodException();
                    }

                    return InvokeMemberByType(tAny, methodName, invokeAttr, args, target);
                }

            }
            catch (TargetInvocationException targetEx)
            {
                if (targetEx.InnerException != null)
                {
                    throw targetEx.InnerException;
                }
            }

            return null;

        }


        public object InvokeMember(object assemblyName, string typeName, string methodName, int invokeAttr, object args, object target)
        {
            try
            {
                Assembly assembly = assemblyName as Assembly;
                if (assembly == null)
                {
                    if(assemblyName!=null)  assembly = this.loadAssembly(assemblyName as string);
                }
                

                Type tAny = null;

                if (assembly != null) {
                    tAny = assembly.GetType(typeName);
                }
                else
                {
                    tAny = Type.GetType(typeName);
                }

                if (((BindingFlags)invokeAttr & BindingFlags.CreateInstance) == BindingFlags.CreateInstance)
                {

                    if (tAny != null && tAny.IsClass)
                    {
                        return CreateInstanceByClassType(tAny, (args as ArrayList), target);
                    }

                }

                if (((BindingFlags)invokeAttr & BindingFlags.InvokeMethod) == BindingFlags.InvokeMethod)
                {
                    if (tAny == null)
                    {
                        if (assembly != null)
                        {
                            tAny = assembly.GetType(typeName + "." + methodName);
                        }
                        else
                        {
                            tAny = Type.GetType(typeName + "." + methodName);
                        }

                        if (tAny != null && tAny.IsClass && ((invokeAttr & (16 | 8 | 256)) == (16 | 8 | 256)))
                        {
                            return CreateInstanceByClassType(tAny, (args as ArrayList), target);
                        }
                    }

                    if (tAny == null) throw new MissingMethodException();
                }

                return InvokeMemberByType(tAny, methodName, invokeAttr, args, target); 

            }
            catch (TargetInvocationException targetEx)
            {
                if (targetEx.InnerException != null)
                {
                    throw targetEx.InnerException;
                }
            }

            return null;

        }

        public object InvokeObjectMember(object target, string methodName, int invokeAttr, object args)
        {
            if (target == null)
            {
                return null;
            }

            try
            {
                return InvokeMemberByType(target.GetType(), methodName, invokeAttr, args, target);
            }
            catch (TargetInvocationException targetEx)
            {
                if (targetEx.InnerException != null) {
                    
                    if (targetEx.InnerException is MissingMethodException)
                    {
                        if( CheckObjectMethod(target, methodName, invokeAttr))
                        {
                            if (System.Threading.Thread.CurrentThread.CurrentCulture.Name.StartsWith("zh", StringComparison.OrdinalIgnoreCase))
                            {
                                throw new ArgumentException($"找到了函数'{methodName}'，但是调用参数的类型不匹配。");
                            }
                            else
                            {
                                throw new ArgumentException($"Method '{methodName}' found but no overload matches the provided parameter types.");
                            }
                        }
                    }
              
                    throw targetEx.InnerException;
                }
            }
            catch(MissingMethodException e){
                if (CheckObjectMethod(target, methodName, invokeAttr))
                { 
                    if (System.Threading.Thread.CurrentThread.CurrentCulture.Name.StartsWith("zh", StringComparison.OrdinalIgnoreCase))
                    { 
                        throw new ArgumentException($"找到了函数'{methodName}'，但是调用参数的类型不匹配。");
                    }
                    else
                    { 
                        throw new ArgumentException($"Method '{methodName}' found but no overload matches the provided parameter types.");
                    }
                }

                throw e;
            }

            return null;
        } 
         public bool CheckObjectMember(object target, string propName, int invokeAttr)
        {

            try
            {
                Type tAny = target.GetType();
                if ((tAny == DispatchableObjectType))
                {
                    DispatchableObject dispObj = (target as DispatchableObject);
                    target = dispObj.Value;
                    tAny = target.GetType();
                }

                var memberInfo = tAny.GetMember(propName, (BindingFlags)invokeAttr);
                return (memberInfo.Length > 0);
            } 
            catch (Exception)
            {
                return false;
            }
        }

        public bool CheckObjectMethod(object target, string methodName, int invokeAttr)
        {

            try
            {
                Type tAny = target.GetType();
                if ((tAny == DispatchableObjectType) )
                {
                    DispatchableObject dispObj = (target as DispatchableObject);
                    target = dispObj.Value;
                    tAny = target.GetType();
                }

                var method = tAny.GetMethod(methodName, (BindingFlags)invokeAttr);
                return (method != null);
            }
            catch(AmbiguousMatchException)
            {
                return true;
            }
            catch( Exception)
            {
                return false;
            }  
        }
         

        public object RoundTrip(object target)
        {  
            return RoundTripRaw(target);
        } 

        [ComVisible(false)]
        public object RoundTripRaw(object target)
        {
            Type tAny = target.GetType();
            if  (tAny == DispatchableObjectType  )
            {
                target = (target as DispatchableObject).Value; 
            }

            return WrapNonPrimitiveValueToAnyObjectRef(target);
        }

        public object InvokeMemberByType(Type tAny, string methodName, int invokeAttr, object args, object target)
        {
            ArrayList argArrayList = (args as ArrayList);
            if ( (tAny == DispatchableObjectType) &&  (target != null)  )
            {
                DispatchableObject dispObj = (target as DispatchableObject);
                target = dispObj.Value;
                tAny = target.GetType();

                if((((BindingFlags)invokeAttr & BindingFlags.InvokeMethod)!= BindingFlags.InvokeMethod) && (methodName == "Value") )
                {
                    Type tEle = tAny;
                    if (tAny.IsArray)
                    {
                        tEle = tAny.GetElementType();

                        if (tEle == typeof(object))
                        {
                            if (((BindingFlags)invokeAttr & BindingFlags.GetField) == BindingFlags.GetField)
                            {
                                return target;
                            }
                            else if (((BindingFlags)invokeAttr & BindingFlags.SetField) == BindingFlags.SetField)
                            {
                                dispObj.Value = argArrayList[0];
                                return null;
                            }
                        }
                    }

                    if (tEle.IsPrimitive || tEle.IsEnum || tEle.IsArray || (tEle == typeof(string)) )
                        {
                            if (((BindingFlags)invokeAttr & BindingFlags.GetField) == BindingFlags.GetField)
                            {
                                return target;
                            }
                            else if (((BindingFlags)invokeAttr & BindingFlags.SetField) == BindingFlags.SetField)
                            {
                                if (argArrayList[0].GetType().Equals(tAny)) dispObj.Value = argArrayList[0];
                                else dispObj.Value = Convert.ChangeType(argArrayList[0], tAny);
                                return null;
                            }
                        }
                    
                    if(tEle == typeof(System.DateTime))
                    {
                        if (((BindingFlags)invokeAttr & BindingFlags.GetField) == BindingFlags.GetField)
                        {
                            System.DateTime t = (System.DateTime)target;
                            if (t.Kind == DateTimeKind.Utc)
                            {
                                return t.ToLocalTime();
                            }
                            return t;
                        }
                        else if (((BindingFlags)invokeAttr & BindingFlags.SetField) == BindingFlags.SetField)
                        {
                            if (argArrayList[0].GetType().Equals(tAny)) dispObj.Value = argArrayList[0];
                            else dispObj.Value = Convert.ChangeType(argArrayList[0], tAny);
                            return null;
                        }
                    }
                }
            }

            if ((argArrayList == null) )
            {
                //不需要匹配参数类型，不需要自动转换。
                return WrapNonPrimitiveValueToAnyObjectRef(tAny.InvokeMember(methodName, (BindingFlags)invokeAttr | BindingFlags.IgnoreReturn, null, target, null));
            }

            //if ((argArrayList == null) || (argArrayList.Count == 0) )
            if (argArrayList == null)
            {
                //不需要匹配参数类型，不需要自动转换。
                return WrapNonPrimitiveValueToAnyObjectRef(tAny.InvokeMember(methodName, (BindingFlags)invokeAttr | BindingFlags.IgnoreReturn, null, target, null));
            }

            if (argArrayList.Count == 0)
            {
                MethodInfo m = null;

                try
                {
                    m = tAny.GetMethod(methodName, (BindingFlags)invokeAttr | BindingFlags.IgnoreReturn, null, new Type[0] { }, null);
                }
                catch (SystemException)
                {

                }


                if (m != null)
                {
                    object ret2 = WrapNonPrimitiveValueToAnyObjectRef(m.Invoke(target, null)); 
                    return ret2;
                }
            }



            if (((BindingFlags)invokeAttr & BindingFlags.InvokeMethod) == BindingFlags.InvokeMethod)
            {
                bool hasOutValues = false;
                object[] outArgs  = new object[argArrayList.Count];
                Type[] argTypeArray = new Type[argArrayList.Count];
                for (int i = 0; i < argArrayList.Count; i++)
                {
                    argTypeArray[i] = argArrayList[i] != null ? argArrayList[i].GetType() : null;
                    if (argTypeArray[i] == DispatchableObjectType) {
                        DispatchableObject v = (argArrayList[i] as DispatchableObject);
                        if (v.ByRef)
                        {
                            outArgs[i] = argArrayList[i];
                            hasOutValues = true;
                        }

                        argArrayList[i] = v.Value;
                    }
                }
                if (!hasOutValues) outArgs = null;

                MethodInfo m = null;
                try
                {
                    m = tAny.GetMethod(methodName, (BindingFlags)invokeAttr | BindingFlags.IgnoreReturn, null, argTypeArray, null);
                }
                catch (SystemException)
                {

                }

                if (m != null)
                {
                    object []invokeArgs3 = argArrayList != null ? argArrayList.ToArray() : null;
                    object ret2 = WrapNonPrimitiveValueToAnyObjectRef( m.Invoke(target, invokeArgs3) );
                    if (outArgs != null) setOutValue(outArgs, invokeArgs3);
                    return ret2;
                } 

                 
                MethodInfo[] ms = tAny.GetMethods((BindingFlags)invokeAttr | BindingFlags.IgnoreReturn);
                bool failed = true;

                object[] invokeArgs2 = null;
                object ret = InvokeMemberBaseMethodInfo(methodName, ms, argArrayList, ref failed, target, invokeAttr, ref invokeArgs2);
                if (!failed)
                { 
                    ret = WrapNonPrimitiveValueToAnyObjectRef(ret);
                    if ((outArgs != null) && ( invokeArgs2!=null ) ) setOutValue(outArgs, invokeArgs2);
                    return ret;
                } 
            }


            if (((BindingFlags)invokeAttr & BindingFlags.GetProperty) == BindingFlags.GetProperty)
            {
                for (int i = 0; i < argArrayList.Count; i++)
                {
                    if (argArrayList[i] is DispatchableObject v)
                    {
                        argArrayList[i] = v.Value;
                    }
                }

                ArrayList methodList = new ArrayList();
                try
                {
                    PropertyInfo prop = tAny.GetProperty(methodName, (BindingFlags)(invokeAttr & ~(int)BindingFlags.GetField));
                    if(prop!=null) methodList.Add(prop.GetGetMethod());
                }
                catch (AmbiguousMatchException)
                {
                    PropertyInfo[] props = tAny.GetProperties((BindingFlags)(invokeAttr & ~(int)BindingFlags.GetField));

                    for (int i = 0; i < props.Length; i++)
                    {
                        if (props[i].Name == methodName)
                        {
                            methodList.Add(props[i].GetGetMethod());
                        }
                    }
                }

                if (methodList.Count > 0)
                {

                    MethodInfo[] ms = new MethodInfo[methodList.Count];
                    for (int i = 0; i < ms.Length; i++)
                    {
                        ms[i] = methodList[i] as MethodInfo;
                    }

                    bool failed = true;
                    object[] invokeArgs2 = null;
                    object  ret2 = InvokeMemberBaseMethodInfo(ms[0].Name, ms, argArrayList, ref failed, target, invokeAttr & ~(int)BindingFlags.GetField, ref invokeArgs2);
                    if (!failed)
                    {
                        return WrapNonPrimitiveValueToAnyObjectRef(ret2);
                    }
                }
            }

            if (((BindingFlags)invokeAttr & BindingFlags.SetProperty) == BindingFlags.SetProperty)
            { 
                for (int i = 0; i < argArrayList.Count; i++)
                { 
                    if (argArrayList[i] is DispatchableObject v)
                    {  
                        argArrayList[i] = v.Value; 
                    }
                }

                PropertyInfo prop = null;
                try
                {
                    prop = tAny.GetProperty(methodName, (BindingFlags)invokeAttr);
                }
                catch(AmbiguousMatchException)
                {
                    ArrayList arrList = new ArrayList();
                    PropertyInfo []props = tAny.GetProperties( (BindingFlags)invokeAttr);
                    
                    for (int i = 0; i < props.Length; i++)
                    { 
                        if (props[i].Name == methodName)
                        { 
                            arrList.Add(props[i].GetSetMethod());
                        }
                    }

                    if (arrList.Count > 0) {

                        MethodInfo[] ms = new MethodInfo[arrList.Count];
                        for (int i = 0; i < ms.Length; i++)
                        {
                            ms[i] = arrList[i] as MethodInfo; 
                        }
                         
                        bool failed = true; 
                        object[] invokeArgs2 = null;
                        InvokeMemberBaseMethodInfo(ms[0].Name, ms, argArrayList, ref failed, target, invokeAttr, ref invokeArgs2);
                        if (!failed)
                        {
                            return null;
                        }
                    }

                }

                if (prop != null)
                {
                    invokeAttr = invokeAttr & ~(int)BindingFlags.SetField;
                    var lastIndex = argArrayList.Count - 1;
                    if (  argArrayList[lastIndex] != null)
                    { 
                        MethodInfo ms = prop.GetSetMethod(); 

                        var parameters = ms.GetParameters(); 
                        if (parameters.Length == argArrayList.Count)
                        { 
                            var paramType = parameters[lastIndex].ParameterType;
                            Type nullableUnderlyingType = Nullable.GetUnderlyingType(paramType);
                            if (nullableUnderlyingType != null)
                            {
                                paramType = nullableUnderlyingType;
                            }
                            //string a = paramType.Name;

                            var argType = argArrayList[lastIndex].GetType();
                            if (argType == DispatchableObjectType)
                            {
                                argArrayList[lastIndex] = (argArrayList[lastIndex] as DispatchableObject).Value;
                                argType = argArrayList[lastIndex].GetType();
                            }
                            var argTypeCode = Type.GetTypeCode(argType);

                            Type enumType = null;
                            if (paramType.IsEnum)
                            {
                                enumType = paramType;
                                paramType = Enum.GetUnderlyingType(enumType);
                            }

                            if (!paramType.IsAssignableFrom(argType))
                            {
                                if (typeof(Delegate).IsAssignableFrom(paramType) && argType.IsCOMObject )
                                {
                                    var dd = new DispatchableDelegate(argArrayList[lastIndex]);
                                    argArrayList[lastIndex] = (object)dd.CreateDelegate(paramType);
                                }
                                else if (argTypeCode == TypeCode.Double || argTypeCode == TypeCode.Int32)
                                {

                                    if (IsNumericType(paramType))
                                    {
                                        argArrayList[lastIndex] = Convert.ChangeType(argArrayList[lastIndex], paramType);
                                    }
                                    else if (paramType == ColorType)
                                    {
                                        argArrayList[lastIndex] = ConvertNumToColor(argArrayList[lastIndex], argTypeCode);
                                    }
                                    else if (paramType == IntPtrType && argTypeCode == TypeCode.Int32 )
                                    {
                                        argArrayList[lastIndex] = new System.IntPtr( (int) argArrayList[lastIndex]  );
                                    }
                                    else if (paramType == UIntPtrType && argTypeCode == TypeCode.Int32)
                                    {
                                        argArrayList[lastIndex] = new  System.UIntPtr( (uint)(int) argArrayList[lastIndex]);
                                    }
                                    else if (paramType == CharType){
                                        argArrayList[lastIndex] =  (char)(int)(argArrayList[lastIndex]);
                                    }
                                }
                                else if (paramType.IsArray && argType.IsArray)
                                {
                                    var paramEleType = paramType.GetElementType();
                                    var argEleType = argType.GetElementType();
                                    var argEleTypeCode = Type.GetTypeCode(argEleType);

                                    if ((argEleTypeCode == TypeCode.Double) && IsNumericType(paramEleType))
                                    {

                                        var srcArr = (argArrayList[lastIndex] as double[]);
                                        var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                        for (int n = 0; n < srcArr.Length; n++)
                                        {
                                            dstArr.SetValue(Convert.ChangeType(srcArr[n], paramEleType), n);
                                        }

                                        argArrayList[lastIndex] = dstArr;
                                    }
                                    else if ((argEleTypeCode == TypeCode.UInt16) && (paramEleType == CharType))
                                    {
                                        var srcArr = (argArrayList[lastIndex] as ushort[]);
                                        var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                        for (int n = 0; n < srcArr.Length; n++)
                                        {
                                            dstArr.SetValue(Convert.ToChar(srcArr[n]), n);
                                        }

                                        argArrayList[lastIndex] = dstArr;
                                    }
                                    else if ((argEleTypeCode == TypeCode.Double) && (paramEleType == ColorType))
                                    {
                                        var srcArr = (argArrayList[0] as double[]);
                                        var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                        for (int n = 0; n < srcArr.Length; n++)
                                        {
                                            
                                            dstArr.SetValue(ConvertNumToColor(srcArr[n], argEleTypeCode), n);
                                        }

                                        argArrayList[0] = dstArr;
                                    }

                                }
                                else if (paramType == CharType)
                                {
                                    if (argTypeCode == TypeCode.String)
                                    { 
                                        argArrayList[lastIndex] = ((string)argArrayList[lastIndex])[0]; 
                                    }
                                }

                            }

                            if (enumType != null) argArrayList[lastIndex] = Enum.ToObject(enumType, argArrayList[lastIndex]);
                        }

                    }
                }
            }



            if (((BindingFlags)invokeAttr & BindingFlags.SetField) == BindingFlags.SetField)
            {
                /*
                for (int i = 0; i < argArrayList.Count; i++)
                {
                    if (argArrayList[i] is DispatchableObject v)
                    {
                        argArrayList[i] = v.Value;
                    }
                }
                */

                FieldInfo field = tAny.GetField(methodName, (BindingFlags)invokeAttr);
                if (field != null)
                {
                    invokeAttr = invokeAttr & ~(int)BindingFlags.SetProperty;

                    if (  argArrayList[0] != null)
                    {
                        var paramType = field.FieldType;
                        Type nullableUnderlyingType = Nullable.GetUnderlyingType(paramType);
                        if (nullableUnderlyingType != null)
                        {
                            paramType = nullableUnderlyingType;
                        }

                        var argType = argArrayList[0].GetType();
                        if (argType == DispatchableObjectType)
                        {
                            argArrayList[0] = (argArrayList[0] as DispatchableObject).Value;
                            argType = argArrayList[0].GetType();
                        }

                        var argTypeCode = Type.GetTypeCode(argType);

                        Type enumType = null;
                        if (paramType.IsEnum)
                        {
                            enumType = paramType;
                            paramType = Enum.GetUnderlyingType(enumType);
                        }

                        if (!paramType.IsAssignableFrom(argType))
                        {

                            if (typeof(Delegate).IsAssignableFrom(paramType) && argType.IsCOMObject)
                            {
                                var dd = new DispatchableDelegate(argArrayList[0]);
                                argArrayList[0] = (object)dd.CreateDelegate(paramType);
                            }
                            else if ((argTypeCode == TypeCode.Double) || (argTypeCode == TypeCode.Int32))
                            {

                                if (IsNumericType(paramType))
                                {
                                    argArrayList[0] = Convert.ChangeType(argArrayList[0], paramType);
                                }
                                else if (paramType == ColorType)
                                {
                                    argArrayList[0] = ConvertNumToColor(argArrayList[0], argTypeCode);
                                }
                                else if (paramType == IntPtrType && argTypeCode == TypeCode.Int32)
                                {
                                    argArrayList[0] = new System.IntPtr((int)argArrayList[0]);
                                }
                                else if (paramType == UIntPtrType && argTypeCode == TypeCode.Int32)
                                {
                                    argArrayList[0] = new System.UIntPtr((uint)(int)argArrayList[0]);
                                }
                                else if (paramType == CharType){
                                    argArrayList[0] =  (char)(int)(argArrayList[0]);
                                }
                            }
                            else if (paramType.IsArray && argType.IsArray)
                            {
                                var paramEleType = paramType.GetElementType();
                                var argEleType = argType.GetElementType();
                                var argEleTypeCode = Type.GetTypeCode(argEleType);

                                if ((argEleTypeCode == TypeCode.Double) && IsNumericType(paramEleType))
                                {

                                    var srcArr = (argArrayList[0] as double[]);
                                    var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                    for (int n = 0; n < srcArr.Length; n++)
                                    {
                                        dstArr.SetValue(Convert.ChangeType(srcArr[n], paramEleType), n);
                                    }

                                    argArrayList[0] = dstArr;
                                }
                                else if ((argEleTypeCode == TypeCode.UInt16) && (paramEleType == CharType))
                                {
                                    var srcArr = (argArrayList[0] as ushort[]);
                                    var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                    for (int n = 0; n < srcArr.Length; n++)
                                    {
                                        dstArr.SetValue(Convert.ToChar(srcArr[n]), n);
                                    }

                                    argArrayList[0] = dstArr;
                                }
                                else if ((argEleTypeCode == TypeCode.Double) && (paramEleType == ColorType))
                                {
                                    var srcArr = (argArrayList[0] as double[]);
                                    var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                    for (int n = 0; n < srcArr.Length; n++)
                                    {
                                        
                                        dstArr.SetValue(ConvertNumToColor(srcArr[n], argEleTypeCode), n);
                                    }

                                    argArrayList[0] = dstArr;
                                }

                            }
                            else if (paramType == CharType)
                            {
                                if (argTypeCode == TypeCode.String)
                                { 
                                    argArrayList[0] = ((string)argArrayList[0])[0]; 
                                }
                            }

                        }

                        if (enumType != null) argArrayList[0] = Enum.ToObject(enumType, argArrayList[0]);
                    }
                }
            }

            if (((BindingFlags)invokeAttr & (BindingFlags.SetProperty | BindingFlags.SetField) ) == (BindingFlags.SetProperty | BindingFlags.SetField))
            {
                EventInfo eventInfo = tAny.GetEvent(methodName);
                if (eventInfo != null)
                {
                    Type handlerType = eventInfo.EventHandlerType;
                    //MethodInfo invokeMethod = handlerType.GetMethod("Invoke");

                    if(!(typeof(Delegate).IsAssignableFrom(argArrayList[0].GetType().BaseType)) )
                    {
                        var dd = new DispatchableDelegate(argArrayList[0]);
                        eventInfo.AddEventHandler(target, dd.CreateDelegate(handlerType));
                    }
                    else
                    {
                        eventInfo.AddEventHandler(target,(Delegate) argArrayList[0]);
                    }
                    

                    return null;
                }
            }

            if ((methodName == "Item") && (target is Array arr)  )
            {  
                if (((BindingFlags)invokeAttr & BindingFlags.GetProperty) == BindingFlags.GetProperty)
                {
                    if (argArrayList.Count == 1) return WrapNonPrimitiveValueToAnyObjectRef(arr.GetValue((int)argArrayList[0]));
                    else if (argArrayList.Count > 1)
                    {
                        object [] objIndices = argArrayList.ToArray();
                        int[] indices = new int[objIndices.Length];
                        for (int i = 0; i < indices.Length; i++)
                        { 
                            indices[i] = ((int)objIndices[i]); 
                        } 
                         
                        return WrapNonPrimitiveValueToAnyObjectRef(arr.GetValue(indices));
                    }
                }
                else if (((BindingFlags)invokeAttr & BindingFlags.SetProperty) == BindingFlags.SetProperty)
                {

                    if (argArrayList.Count == 2) {
                        if( tAny.GetElementType() != ColorType)
                        {
                            arr.SetValue(argArrayList[1], (int)argArrayList[0]);
                        }
                        else
                        {
                            // 在 aardio 中用下标设置 System.Drawing.Color 数组的值
                            object value = argArrayList[1];
                            System.Drawing.Color clrValue =  ConvertNumToColor(value, Type.GetTypeCode(value.GetType()));
                            arr.SetValue(clrValue, (int)argArrayList[0]);
                        }

                    }
                    else if(argArrayList.Count > 2)
                    {
                        object[] objIndices = argArrayList.ToArray();
                        int[] indices = new int[objIndices.Length-1];
                        for (int i = 0; i < indices.Length; i++)
                        { 
                            indices[i] = ((int)objIndices[i]);
                        }

                        if( tAny.GetElementType() != ColorType){
                            arr.SetValue(argArrayList[indices.Length], indices);
                        }
                        else
                        {
                            object value = argArrayList[indices.Length];
                            System.Drawing.Color clrValue =  ConvertNumToColor(value, Type.GetTypeCode(value.GetType()));
                            arr.SetValue(clrValue, indices);
                        }
                    }
                    return null;
                } 
            }

            return WrapNonPrimitiveValueToAnyObjectRef( tAny.InvokeMember(methodName, (BindingFlags)invokeAttr | BindingFlags.IgnoreReturn, null, target, (argArrayList != null) ? argArrayList.ToArray() : null) ); 
        }

        private object CreateInstanceByClassType(Type tClass, ArrayList argArrayList, object target)
        {
            if( argArrayList == null )
            {
                return WrapNonPrimitiveValueToAnyObjectRef(tClass.InvokeMember("", (BindingFlags)BindingFlags.CreateInstance | BindingFlags.IgnoreReturn, null, target, null));
            }


            if (argArrayList.Count == 0)
            {
                MethodInfo m2 = null;

                try
                {
                    m2 = tClass.GetMethod("", (BindingFlags)BindingFlags.CreateInstance | BindingFlags.IgnoreReturn, null, new Type[0] { }, null);
                }
                catch (SystemException)
                {

                }

                if (m2 != null)
                {
                    return WrapNonPrimitiveValueToAnyObjectRef(m2.Invoke(target, null));  
                }
            }
        
            Type[] argTypeArray = new Type[argArrayList.Count]; 

            bool hasOutValues = false;
            object[] outArgs = new object[argArrayList.Count]; 
            for (int i = 0; i < argArrayList.Count; i++)
            {
                argTypeArray[i] = argArrayList[i] != null ? argArrayList[i].GetType() : null;
                if (argTypeArray[i] == DispatchableObjectType)
                {
                    DispatchableObject v = (argArrayList[i] as DispatchableObject);
                    if (v.ByRef)
                    {
                        outArgs[i] = argArrayList[i];
                        hasOutValues = true;
                    }

                    argArrayList[i] = v.Value;
                }
            }
            if (!hasOutValues) outArgs = null;

            ConstructorInfo m = null;
            try
            {
                m = tClass.GetConstructor(argTypeArray);
            }
            catch (SystemException)
            {

            }

            if (m != null)
            {
                object[] invokeArgs = argArrayList.ToArray();
                object ret = m.Invoke(invokeArgs);
                if (outArgs != null) setOutValue(outArgs, invokeArgs);
                return WrapNonPrimitiveValueToAnyObjectRef(ret);
            }

            ConstructorInfo[] ms = tClass.GetConstructors();
            if(ms.Length==0 && tClass.IsValueType)
            {
                return WrapNonPrimitiveValueToAnyObjectRef( Activator.CreateInstance(tClass) );
            }

            bool failed = true;
            object[] invokeArgs2 = null;
            object ret2 = InvokeMemberBaseMethodInfo(".ctor", ms, argArrayList, ref failed, target, (int)BindingFlags.CreateInstance,ref invokeArgs2);
            if (!failed)
            {
                if (outArgs != null) setOutValue(outArgs, invokeArgs2);
                return WrapNonPrimitiveValueToAnyObjectRef(ret2);
            }

            invokeArgs2 = argArrayList.ToArray();
            ret2 = tClass.InvokeMember("", (BindingFlags)BindingFlags.CreateInstance | BindingFlags.IgnoreReturn, null, target, invokeArgs2);
            if (outArgs != null) setOutValue(outArgs, invokeArgs2);
            return WrapNonPrimitiveValueToAnyObjectRef(ret2);
        }

        private static bool IsNumericType(Type t)
        {
            switch (Type.GetTypeCode(t))
            {
                case TypeCode.Byte:
                case TypeCode.SByte:
                case TypeCode.UInt16:
                case TypeCode.UInt32:
                case TypeCode.UInt64:
                case TypeCode.Int16:
                case TypeCode.Int32:
                case TypeCode.Int64:
                case TypeCode.Decimal:
                case TypeCode.Double:
                case TypeCode.Single:
                    return true;
                default:
                    return false;
            }
        }
        
        static System.Drawing.Color ConvertNumToColor(object arg, TypeCode argTypeCode)
        {
            byte[] bytes;
            if (argTypeCode == TypeCode.Double) bytes = BitConverter.GetBytes((uint)Convert.ChangeType(arg, typeof(uint)));
            else bytes = BitConverter.GetBytes((int)arg);

            return System.Drawing.Color.FromArgb(bytes[3], bytes[2], bytes[1], bytes[0]);
        }

        private object InvokeMemberBaseMethodInfo(string methodName, MethodBase[] ms, ArrayList argArrayList, ref bool failed, object target, int invokeAttr, ref object[] invokeArgs2)
        {

            for (int i = 0; i < ms.Length; i++)
            {
                if ((ms[i].Name != methodName) || ms[i].IsGenericMethodDefinition) continue;
                var parameters = ms[i].GetParameters();

                if (parameters.Length == argArrayList.Count)
                {
                    failed = false;
                    for (int k = 0; k < parameters.Length; k++)
                    {
                        var paramType = parameters[k].ParameterType;
                        Type nullableUnderlyingType = Nullable.GetUnderlyingType(paramType);
                        if (argArrayList[k] == null)
                        {
                            if (!paramType.IsValueType || nullableUnderlyingType != null)
                                continue;

                            failed = true;
                            break;
                        }

                        if (nullableUnderlyingType != null)
                        {
                            paramType = nullableUnderlyingType;
                        }

                        var argType = argArrayList[k].GetType();
                        if (paramType.IsAssignableFrom(argType)) continue;

                        Type enumType = null;
                        if (paramType.IsEnum)
                        {
                            enumType = paramType;
                            paramType = Enum.GetUnderlyingType(enumType);
                        }

                        //string a = paramType.Name;
                        if (!paramType.IsAssignableFrom(argType))
                        {
                            if (typeof(Delegate).IsAssignableFrom(paramType) && (argType == null) || argType.IsCOMObject)
                            {
                                if (argType != null)
                                {
                                    var dd = new DispatchableDelegate(argArrayList[k]);
                                    argArrayList[k] = (object)dd.CreateDelegate(paramType);
                                }
                                
                            }
                            else
                            {
                                failed = true;
                                break;
                            }
                        }
                        else if (enumType != null) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                    }

                    if (!failed)
                    {
                        invokeArgs2 = argArrayList.ToArray();
                        if (methodName == ".ctor") return (ms[i] as ConstructorInfo).Invoke(invokeArgs2);
                        return ms[i].Invoke(target, (BindingFlags)invokeAttr, null, invokeArgs2, null);
                    }
                }
            }

            for (int i = 0; i < ms.Length; i++)
            {
                if ((ms[i].Name != methodName) || ms[i].IsGenericMethodDefinition) continue;

                var parameters = ms[i].GetParameters();
                if (parameters.Length > argArrayList.Count)
                {
                    failed = false;
                    for (int k = 0; k < argArrayList.Count; k++)
                    {
                        var paramType = parameters[k].ParameterType;
                        Type nullableUnderlyingType = Nullable.GetUnderlyingType(paramType);
                        if (argArrayList[k] == null)
                        {
                            if (!paramType.IsValueType || nullableUnderlyingType != null)
                                continue;

                            failed = true;
                            break;
                        }

                        if (nullableUnderlyingType != null)
                        {
                            paramType = nullableUnderlyingType;
                        }

                        var argType = argArrayList[k].GetType();
                        if (paramType.IsAssignableFrom(argType)) continue;

                        Type enumType = null;
                        if (paramType.IsEnum)
                        {
                            enumType = paramType;
                            paramType = Enum.GetUnderlyingType(enumType);
                        }

                        if (!paramType.IsAssignableFrom(argType))
                        {
                            if (typeof(Delegate).IsAssignableFrom(paramType) && (argType == null) || argType.IsCOMObject)
                            {
                                if (argType != null)
                                {
                                    var dd = new DispatchableDelegate(argArrayList[k]);
                                    argArrayList[k] = (object)dd.CreateDelegate(paramType);
                                }  
                            }
                            else
                            {
                                failed = true;
                                break;
                            }
                        }
                        else if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                    }

                    ArrayList argList2 = argArrayList.Clone() as ArrayList;
                    for (int k = argArrayList.Count; k < parameters.Length; k++)
                    {
                        if (!parameters[k].IsOptional)
                        {
                            failed = true;
                            break;
                        }

                        argList2.Add(parameters[k].DefaultValue);
                    }

                    if (!failed)
                    {
                        invokeArgs2 = argList2.ToArray();
                        if (methodName == ".ctor") return (ms[i] as ConstructorInfo).Invoke(invokeArgs2);
                        return ms[i].Invoke(target, (BindingFlags)invokeAttr, null, invokeArgs2, null);
                    }
                }
            }

            for (int i = 0; i < ms.Length; i++)
            {
                if ((ms[i].Name != methodName) || ms[i].IsGenericMethodDefinition) continue;

                var parameters = ms[i].GetParameters();
                if (parameters.Length == argArrayList.Count)
                {
                    failed = false;
                    for (int k = 0; k < parameters.Length; k++)
                    {
                        var paramType = parameters[k].ParameterType;
                        Type nullableUnderlyingType = Nullable.GetUnderlyingType(paramType);
                        if (argArrayList[k] == null)
                        {
                            if (!paramType.IsValueType || nullableUnderlyingType != null)
                                continue;

                            failed = true;
                            break;
                        }

                        if (nullableUnderlyingType != null)
                        {
                            paramType = nullableUnderlyingType;
                        }

                        var argType = argArrayList[k].GetType();
                        if (paramType.IsAssignableFrom(argType)) continue;

                        Type enumType = null;
                        if (paramType.IsEnum)
                        {
                            enumType = paramType;
                            paramType = Enum.GetUnderlyingType(enumType);
                        }

                        //string a = paramType.Name;
                        if (!paramType.IsAssignableFrom(argType))
                        {
                            if (typeof(Delegate).IsAssignableFrom(paramType) && (argType == null) || argType.IsCOMObject)
                            {
                                if (argType != null)
                                {
                                    var dd = new DispatchableDelegate(argArrayList[k]);
                                    argArrayList[k] = (object)dd.CreateDelegate(paramType);
                                }
                                continue;
                            }

                            var paramTypeCode = Type.GetTypeCode(paramType);
                            var argTypeCode = Type.GetTypeCode(argType);

                            if (argTypeCode == TypeCode.Double || argTypeCode == TypeCode.Int32)
                            {

                                if (IsNumericType(paramType))
                                {
                                    argArrayList[k] = Convert.ChangeType(argArrayList[k], paramType);
                                    if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                    continue;
                                }
                                else if (paramType == ColorType)
                                {
                                    argArrayList[k] = ConvertNumToColor(argArrayList[k], argTypeCode);
                                    if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                    continue;
                                }
                                else if (paramType == IntPtrType && argTypeCode == TypeCode.Int32)
                                {
                                    argArrayList[k] = new System.IntPtr((int)argArrayList[k]);
                                    if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                    continue;
                                }
                                else if (paramType == UIntPtrType && argTypeCode == TypeCode.Int32)
                                {
                                    argArrayList[k] = new System.UIntPtr((uint)(int)argArrayList[k]);
                                    if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                    continue;
                                }
                                else if (paramType == CharType)
                                { 
                                    argArrayList[k] =  Convert.ToChar((int)(argArrayList[k]));
                                    if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                    continue;
                                }
                            }
                            else if (paramType.IsArray && argType.IsArray)
                            {
                                var paramEleType = paramType.GetElementType();
                                var argEleType = argType.GetElementType();
                                var argEleTypeCode = Type.GetTypeCode(argEleType); 

                                if ((argEleTypeCode == TypeCode.Double) && IsNumericType(paramEleType))
                                {

                                    var srcArr = (argArrayList[k] as double[]);
                                    var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                    for (int n = 0; n < srcArr.Length; n++)
                                    {
                                        dstArr.SetValue(Convert.ChangeType(srcArr[n], paramEleType), n);
                                    }

                                    argArrayList[k] = dstArr;
                                    continue;
                                }
                                else if((argEleTypeCode == TypeCode.UInt16) && (paramEleType == CharType))
                                {
                                    var srcArr = (argArrayList[k] as ushort[]);
                                    var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                    for (int n = 0; n < srcArr.Length; n++)
                                    {
                                        dstArr.SetValue(Convert.ToChar(srcArr[n]), n);
                                    }

                                    argArrayList[k] = dstArr;
                                    continue;
                                }
                                else if ((argEleTypeCode == TypeCode.Double) && (paramEleType == ColorType))
                                {
                                    var srcArr = (argArrayList[0] as double[]);
                                    var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                    for (int n = 0; n < srcArr.Length; n++)
                                    {
                                        
                                        dstArr.SetValue(ConvertNumToColor(srcArr[n], argEleTypeCode), n);
                                    }

                                    argArrayList[0] = dstArr;
                                }

                            }
                            else if (paramType == CharType)
                            {
                                if (argTypeCode == TypeCode.String)
                                { 
                                    argArrayList[k] = ((string)argArrayList[k])[0]; 
                                    if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                    continue;
                                }
                            }

                            failed = true;
                            break;
                        }

                        if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                    }

                    if (!failed)
                    {
                        invokeArgs2 = argArrayList.ToArray();
                        if (methodName == ".ctor") return (ms[i] as ConstructorInfo).Invoke(invokeArgs2);
                        return ms[i].Invoke(target, (BindingFlags)invokeAttr, null, invokeArgs2, null);
                    }
                }
            }

            for (int i = 0; i < ms.Length; i++)
            {
                if ((ms[i].Name != methodName) || ms[i].IsGenericMethodDefinition) continue;

                var parameters = ms[i].GetParameters();
                if (parameters.Length > argArrayList.Count)
                {
                    failed = false;
                    for (int k = 0; k < argArrayList.Count; k++)
                    {
                        var paramType = parameters[k].ParameterType;
                        Type nullableUnderlyingType = Nullable.GetUnderlyingType(paramType);
                        if (argArrayList[k] == null)
                        {
                            if (!paramType.IsValueType || nullableUnderlyingType != null)
                                continue;

                            failed = true;
                            break;
                        }

                        if (nullableUnderlyingType != null)
                        {
                            paramType = nullableUnderlyingType;
                        }

                        var argType = argArrayList[k].GetType();
                        if (paramType.IsAssignableFrom(argType)) continue;

                        Type enumType = null;
                        if (paramType.IsEnum)
                        {
                            enumType = paramType;
                            paramType = Enum.GetUnderlyingType(enumType);
                        }

                        if (!paramType.IsAssignableFrom(argType))
                        {
                            if (typeof(Delegate).IsAssignableFrom(paramType) && (argType == null) || argType.IsCOMObject)
                            {
                                if (argType != null)
                                {
                                    var dd = new DispatchableDelegate(argArrayList[k]);
                                    argArrayList[k] = (object)dd.CreateDelegate(paramType);
                                }
                            }
                            else
                            {

                                var paramTypeCode = Type.GetTypeCode(paramType);
                                var argTypeCode = Type.GetTypeCode(argType);

                                if (argTypeCode == TypeCode.Double || argTypeCode == TypeCode.Int32)
                                {

                                    if (IsNumericType(paramType))
                                    {
                                        argArrayList[k] = Convert.ChangeType(argArrayList[k], paramType);
                                        if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                        continue;
                                    }
                                    else if (paramType == ColorType)
                                    {
                                        argArrayList[k] = ConvertNumToColor(argArrayList[k], argTypeCode);
                                        if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                        continue;
                                    }
                                    else if (paramType == IntPtrType && argTypeCode == TypeCode.Int32)
                                    {
                                        argArrayList[k] = new System.IntPtr((int)argArrayList[k]);
                                        if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                        continue;
                                    }
                                    else if (paramType == UIntPtrType && argTypeCode == TypeCode.Int32)
                                    {
                                        argArrayList[k] = new System.UIntPtr((uint)(int)argArrayList[k]);
                                        if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                                        continue;
                                    }
                                }
                                else if (paramType.IsArray && argType.IsArray)
                                {
                                    var paramEleType = paramType.GetElementType();
                                    var argEleType = argType.GetElementType();
                                    var argEleTypeCode = Type.GetTypeCode(argEleType);

                                    if ((argEleTypeCode == TypeCode.Double) && IsNumericType(paramEleType))
                                    {

                                        var srcArr = (argArrayList[k] as double[]);
                                        var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                        for (int n = 0; n < srcArr.Length; n++)
                                        {
                                            dstArr.SetValue(Convert.ChangeType(srcArr[n], paramEleType), n);
                                        }

                                        argArrayList[k] = dstArr;
                                        continue;
                                    }
                                    else if ((argEleTypeCode == TypeCode.UInt16) && (paramEleType == CharType))
                                    {
                                        var srcArr = (argArrayList[k] as ushort[]);
                                        var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                        for (int n = 0; n < srcArr.Length; n++)
                                        {
                                            dstArr.SetValue(Convert.ToChar(srcArr[n]), n);
                                        }

                                        argArrayList[k] = dstArr;
                                        continue;
                                    }
                                    else if ((argEleTypeCode == TypeCode.Double) && (paramEleType == ColorType))
                                    {
                                        var srcArr = (argArrayList[0] as double[]);
                                        var dstArr = Array.CreateInstance(paramEleType, srcArr.Length);
                                        for (int n = 0; n < srcArr.Length; n++)
                                        {
                                            
                                            dstArr.SetValue(ConvertNumToColor(srcArr[n], argEleTypeCode), n);
                                        }

                                        argArrayList[0] = dstArr;
                                    }

                                }

                                failed = true;
                                break;
                            }
                        }
                        else if (enumType != null && !(argType.IsEnum)) argArrayList[k] = Enum.ToObject(enumType, argArrayList[k]);
                    }

                    ArrayList argList2 = argArrayList.Clone() as ArrayList;
                    for (int k = argArrayList.Count; k < parameters.Length; k++)
                    {
                        if (!parameters[k].IsOptional)
                        {
                            failed = true;
                            break;
                        }

                        argList2.Add(parameters[k].DefaultValue);
                    }

                    if (!failed)
                    {
                        invokeArgs2 = argList2.ToArray();
                        if (methodName == ".ctor") return (ms[i] as ConstructorInfo).Invoke(invokeArgs2);
                        return ms[i].Invoke(target, (BindingFlags)invokeAttr, null, invokeArgs2, null);
                    }
                }
            }

            for (int i = 0; i < ms.Length; i++)
            {
                if (ms[i] is MethodInfo method) {  
                    var methodParameters = method.GetParameters(); 
                    if (method.Name == methodName && argArrayList.Count <= methodParameters.Length)
                    {
                        if (!method.IsGenericMethodDefinition) continue;

                        var genericArguments = method.GetGenericArguments();
                        var genericParameters = new Type[genericArguments.Length];  
                        int genericArgumentIndex;
                             
                        for (int k = 0; k < argArrayList.Count; k++)
                        {
                            var argType = argArrayList[k].GetType();
                            genericArgumentIndex = Array.IndexOf(genericArguments, methodParameters[k].ParameterType);

                            if (genericArgumentIndex == -1) continue;
                            genericParameters[genericArgumentIndex] = argType;
                        }

                        if (Array.IndexOf(genericParameters, null) != -1)
                        {
                            continue;
                        }

                        if (argArrayList.Count < methodParameters.Length  )
                        {

                            ArrayList argList2 = argArrayList.Clone() as ArrayList;
                            bool isOptionalFailed = false;
                            for (int k = argArrayList.Count; k < methodParameters.Length; k++)
                            {
                                if (!methodParameters[k].IsOptional)
                                {
                                    isOptionalFailed = true;
                                    break;
                                }

                                argList2.Add(methodParameters[k].DefaultValue);
                            }
                            if (isOptionalFailed) continue;

                            var concreteMethod = method.MakeGenericMethod(genericParameters);

                            failed = false;
                            invokeArgs2 = argList2.ToArray();
                            return concreteMethod.Invoke(target, (BindingFlags)invokeAttr, null, invokeArgs2, null);
                        }
                        else
                        {
                            var concreteMethod = method.MakeGenericMethod(genericParameters);

                            failed = false;
                            invokeArgs2 = argArrayList.ToArray();
                            return concreteMethod.Invoke(target, (BindingFlags)invokeAttr, null, invokeArgs2, null);
                        }
                    }
                }
            }


            for (int i = 0; i < ms.Length; i++)
            {
                if (ms[i].Name == methodName && ms[i].GetParameters().Length == argArrayList.Count)
                {
                    if (!ms[i].IsGenericMethodDefinition)
                    { 
                        ParameterInfo[] parameters = ms[i].GetParameters();
                        if (parameters.Length > 0 && Attribute.IsDefined(parameters[parameters.Length - 1], typeof(ParamArrayAttribute)))
                        {
                            failed = true;
                            return null;
                        }


                        failed = false;
                        invokeArgs2 = argArrayList.ToArray();
                        if (methodName == ".ctor") return (ms[i] as ConstructorInfo).Invoke(invokeArgs2);
                        return ms[i].Invoke(target, (BindingFlags)invokeAttr, null, invokeArgs2, null);
                    }
                }
            }
            

            failed = true;
            return null;
        }

        public Object InvokeEnumValue(object enumType, string methodName)
        {
            return System.Enum.Parse(enumType as Type, methodName);
        }

        public Object InvokeEnumValueObject(object enumType, string methodName,Boolean byRef)
        { 
             
            Type actualEnumType = enumType as Type; 
            FieldInfo field = actualEnumType.GetField(methodName);

            if (field != null)
            {
                object enumValueObject = field.GetValue(null);
                return new DispatchableObject(enumValueObject, byRef);
            }

            return null;
        }

        public Object InvokeEnumType(object assemblyName, string nameSpace, string enumTypeName)
        {
            Assembly assembly = assemblyName as Assembly;
            if (assembly == null)
            {
                assembly = this.loadAssembly(assemblyName as string);
            }
            if (assembly != null)
            {
                string fullTypeName = null;
                if (nameSpace != null)
                {
                    try
                    {
                        Type nsType = assembly.GetType(nameSpace);
                        if (nsType.IsClass)
                        {
                            fullTypeName = nameSpace + "+" + enumTypeName;
                        }
                        else
                        {
                            fullTypeName = nameSpace + "." + enumTypeName;
                        }
                    }
                    catch (SystemException)
                    {
                        fullTypeName = nameSpace + "." + enumTypeName;
                    }
                }

                Type t = assembly.GetType(fullTypeName);
                if (t.IsEnum)
                {
                    return t as object;
                }
            }
            return null;
        }


        public Object ParseEnum(object assemblyName, string nameSpace, string enumTypeName, string methodName)
        {
            Assembly assembly = assemblyName as Assembly;
            if (assembly == null)
            {
                assembly = this.loadAssembly(assemblyName as string);
            }
            if (assembly != null)
            {
                string fullTypeName = null;
                if (nameSpace != null)
                {
                    try
                    {
                        Type nsType = assembly.GetType(nameSpace);
                        if (nsType.IsClass)
                        {
                            fullTypeName = nameSpace + "+" + enumTypeName;
                        }
                        else
                        {
                            fullTypeName = nameSpace + "." + enumTypeName;
                        }
                    }
                    catch (SystemException)
                    {
                        fullTypeName = nameSpace + "." + enumTypeName;
                    }
                }

                Type t = assembly.GetType(fullTypeName);
                if (t.IsEnum)
                {
                    return System.Enum.Parse(t, methodName);
                }
            }
            return null;
        }

        public Assembly loadAssembly(string assemblyName)
        {
            try
            {
                Assembly assembly = Assembly.LoadWithPartialName(assemblyName);
                Assembly result = assembly;
                return result;
            }
            catch (SystemException)
            {
            }
            try
            {
                Assembly assembly2 = Assembly.Load(AssemblyName.GetAssemblyName(assemblyName));
                Assembly result = assembly2;
                return result;
            }
            catch (SystemException)
            {
            }
            try
            {
                Assembly assembly3 = Assembly.LoadFrom(assemblyName);
                Assembly result = assembly3;
                return result;
            }
            catch (SystemException)
            {
            }
            try
            {
                Assembly assembly4 = Assembly.LoadFile(assemblyName);
                Assembly result = assembly4;
                return result;
            }
            catch (SystemException)
            {
            }
            return null;
        }

        public class NameValue<TK,TV>
        {
            public TK Name { get; private set; }
            public TV Value { get; private set; }
            public NameValue(TK name, TV value)
            {
                Name = name;
                Value = value;
            } 
        }

        [ ComVisible(false)]
        private object NameValueList2(object nameList, object valueList)
        {
            if (nameList is DispatchableObject n) nameList = n.Value;
            if (valueList is DispatchableObject v) valueList = v.Value;

            Array names = (nameList as Array);
            Array values = (valueList as Array);

            Type NameValueConstructed = typeof(NameValue<,>).MakeGenericType(new Type[] { names.GetValue(0).GetType(), values.GetValue(0).GetType() });
            Type NameValueListConstructed = typeof(List<>).MakeGenericType(new Type[] { NameValueConstructed });

            IList o = (IList)Activator.CreateInstance(NameValueListConstructed);

            for (int i = 0; i < names.Length; i++)
            {
                o.Add( Activator.CreateInstance(NameValueConstructed, new object[] { names.GetValue(i), values.GetValue(i) }));
            }

            return o;
        }

        public object NameValueList(object nameList,object valueList)
        { 

            return NameValueList2(nameList, valueList);
        } 

        public void ReopenConsole(bool utf8)
		{
			if(utf8){
				TextWriter writer = new StreamWriter(Console.OpenStandardOutput())
				{ AutoFlush = true };
				Console.SetOut(writer);
	
				TextWriter writerErr = new StreamWriter(Console.OpenStandardError())
				{ AutoFlush = true };
				Console.SetError(writerErr);
	
				TextReader reader = new StreamReader(Console.OpenStandardInput());
				Console.SetIn(reader);
			}
			else {
				TextWriter writer = new StreamWriter(Console.OpenStandardOutput(),System.Text.Encoding.Default )
				{ AutoFlush = true };
				Console.SetOut(writer);
	
				TextWriter writerErr = new StreamWriter(Console.OpenStandardError(),System.Text.Encoding.Default )
				{ AutoFlush = true };
				Console.SetError(writerErr);
	
				TextReader reader = new StreamReader(Console.OpenStandardInput(),System.Text.Encoding.Default );
				Console.SetIn(reader);
			}
				
		}

    }
}
