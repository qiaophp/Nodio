using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Management.Automation;
using System.Globalization;
using System.Management.Automation.Host;
using System.Management.Automation.Runspaces;
using System.Collections;
using System.Runtime.InteropServices;
using System.Reflection;

namespace aardio
{  
   public class DynObject : IEnumerable, ICollection, IDictionary
    {
        private object target;
        private Type type;
        public DynObject(object obj) { target = obj; type = target.GetType(); }
        IEnumerator IEnumerable.GetEnumerator() { return (target as IEnumerable).GetEnumerator(); } 
        IDictionaryEnumerator IDictionary.GetEnumerator() { return new DynObjectEnumerator(this,(target as IEnumerable).GetEnumerator() ); }

        public object InvokeMember(string method, params object[] args) { 
			for( int i = 0 ;i < args.Length; i++ ) if( args[i] is DynObject ) args[i] = (args[i] as DynObject).Value;
			return type.InvokeMember(method, BindingFlags.InvokeMethod, null, target, args); 
		}
        public object InvokeMember(int dispId, params object[] args) { 
			for( int i = 0 ;i < args.Length; i++ ) if( args[i] is DynObject ) args[i] = (args[i] as DynObject).Value;
			return type.InvokeMember("[DispId=" + dispId + "]", BindingFlags.InvokeMethod, null, target, args); 
		}
        public object Invoke(params object[] args) { 
			for( int i = 0 ;i < args.Length; i++ ) if( args[i] is DynObject ) args[i] = (args[i] as DynObject).Value;
			return type.InvokeMember("", BindingFlags.InvokeMethod, null, target, args); 
		}

  		public object Value
        {
            get { return target;}
		}

        public object this[string index]
        {
            get { 
				object ret = type.InvokeMember(index, BindingFlags.GetProperty, null, target, null); 
				if( ( ret !=  null ) && ( ret.GetType().IsCOMObject) )  return new DynObject(ret);
				return ret;
			}
            set { 
				if( value is DynObject ) value = (value as DynObject).Value;
				type.InvokeMember(index, BindingFlags.SetProperty, null, target, new object[] { value }); 
			}
        }
        
        public object this[object index]
        {
            get { 
				if( index is string ) {
					object ret = type.InvokeMember(index as string, BindingFlags.GetProperty, null, target, null); 
					if( ( ret !=  null ) && ( ret.GetType().IsCOMObject) )  return new DynObject(ret);
					return ret;
				}
				return null;
			}
            set { if( index is string ) type.InvokeMember(index as string, BindingFlags.SetProperty, null, target, new object[] { value }); }
        }

        public static DynObject New(object obj){
  	    	return new DynObject(obj);
  	    }

        public int Count{
            get{
            	int n = 0;
            	foreach (object item in this)
            	{
                	n++;
            	}
	
            	return n;
            }
        }
        

        public object SyncRoot{ get{return this;}}

        public bool IsSynchronized{ get{return false;}}

        public void CopyTo(Array array, int index){
            if (array != null && array.Rank != 1)
            {
                throw new ArgumentException("Multi-dimensional arrays are not supported");
            }   

            foreach (object item in this)
            {
                array.SetValue(item,index++);
            } 
        }
        
 
		public ICollection Keys
		{ 
			get{
				string []keys = new string[this.Count];
				this.CopyTo(keys,0);
				return keys; 
			}
		}
		
 		public ICollection Values
		{
			get{
				object []values = new object[this.Count];
				
				int i = 0;
				foreach (object item in this)
            	{
                	values[i++] = this[item as string];
            	}
				return values; 
			}
		}
 	
		public bool IsReadOnly
		{ 
			get{ return false;}
		}
 	
		public bool IsFixedSize
		{
			get{ return false;}
		}
		
		public bool Contains(object key){
			foreach (object item in this)
        	{
           		if( item as string == key as string ) return true;
        	}	
        	return false;
		}
 	
		public void Add(object key, object value){
			if(key is string) this[key as string] = value;
		}
	
 	
		public void Clear(){
			foreach (object item in this)
        	{
          		this[item as string ] = null;
        	}	
		}
   	
		public void Remove(object key){
			if(key is string ) this[key as string] = null;
    	}

        public class DynObjectEnumerator: IDictionaryEnumerator,IEnumerator{
             IEnumerator enumerator;
             private DynObject dynObj;
             public DynObjectEnumerator(DynObject t,IEnumerator e) {  
             	enumerator =  e;
             	dynObj = t;
             }
             
 			public object Key
			{ 
				get{ return enumerator.Current;}
			}
 	
			public object Value
			{ 
				get{ return dynObj[enumerator.Current as string];}
			}
 		
			public DictionaryEntry Entry
			{ 
				get { return new DictionaryEntry(this.Key,this.Value);}
			}
			
			 public object Current
			{
				get{ return new DictionaryEntry(this.Key,this.Value);}
			}
 
			public bool MoveNext(){ return enumerator.MoveNext();}

 
			public void Reset(){
             	enumerator.Reset();
        	}
        }
    }
}