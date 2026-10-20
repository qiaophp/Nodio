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
    public class PS
    {
        public delegate string OnWriteDelegateType(string value);
        public delegate string OnWriteProgressDelegateType(long sourceId, ProgressRecord record);
        public delegate string OnReadLineDelegateType();

        public static OnWriteDelegateType OnWrite;
        public static OnWriteProgressDelegateType OnWriteProgress;
        public static OnReadLineDelegateType OnReadLine;
        public static OnReadLineDelegateType OnReadLineAsSecureString;

        public static string InvokeScript(string command,  bool useLocalScope)
        { 
            return InvokeScript(command, useLocalScope, new string[0], new string[0]);
        }

        public static string InvokeScript(string command,  bool useLocalScope, string[] parameterNames)
        {
            return InvokeScript(command, useLocalScope, parameterNames, new string[0]);
        }

        public static string InvokeScript(string command,bool useLocalScope, string[] parameterNames, object parameterValue)
        {
            ArrayList parameterValues = (parameterValue as ArrayList);
            CustomPSHost host = new CustomPSHost();
            CustomPSHostUserInterface ui = ((CustomPSHostUserInterface)host.UI);
            ui.OnWrite = OnWrite;
            ui.OnWriteProgress = OnWriteProgress;
            ui.OnReadLine = OnReadLine;
            ui.OnReadLineAsSecureString = OnReadLineAsSecureString;
             
            InitialSessionState state = InitialSessionState.CreateDefault();
            state.AuthorizationManager = null; 

            using (RunspacePool runspacePool = RunspaceFactory.CreateRunspacePool(1,10,host))
            {
                runspacePool.Open();

                PowerShell ps = PowerShell.Create();
                ps.RunspacePool = runspacePool;

                PowerShell nextPsCommand = ps.AddScript(command, useLocalScope); 

                for (int i = 0; i < parameterNames.Length; i++)
                {
                    if ( (parameterValues!=null) &&(i < parameterValues.Count) ) {
                        nextPsCommand = nextPsCommand.AddParameter(parameterNames[i], parameterValues[i]);
                    }
                    else {
                        nextPsCommand = nextPsCommand.AddArgument(parameterNames[i]);
                    }
                }  

                // Now add the default outputter to the end of the pipe and indicate
                // that it should handle both output and errors from the previous
                // commands. This will result in the output being written using the PSHost
                // and PSHostUserInterface classes instead of returning objects to the hosting
                // application.
                nextPsCommand.AddCommand("out-default");
                nextPsCommand.Commands.Commands[0].MergeMyResults(PipelineResultTypes.Error, PipelineResultTypes.Output);
            
                nextPsCommand.Invoke(); //Invoke(new object[] { input })
            }

            string output = ui.Output;
            return output;
        }

        public static string InvokeCommand(string command, bool isScript, bool useLocalScope)
        { 
            return InvokeCommand(command, isScript, useLocalScope, new string[0], new string[0]);
        }

        public static string InvokeCommand(string command, bool isScript, bool useLocalScope, string[] parameterNames)
        {
            return InvokeCommand(command, isScript, useLocalScope, parameterNames, new string[0]);
        }


        public static string InvokeCommand(string command, bool isScript, bool useLocalScope, string[] parameterNames, object parameterValue)
        {
            ArrayList parameterValues = (parameterValue as ArrayList);
            CustomPSHost host = new CustomPSHost();
            CustomPSHostUserInterface ui = ((CustomPSHostUserInterface)host.UI);
            ui.OnWrite = OnWrite;
            ui.OnWriteProgress = OnWriteProgress;
            ui.OnReadLine = OnReadLine;
            ui.OnReadLineAsSecureString = OnReadLineAsSecureString;

            InitialSessionState state = InitialSessionState.CreateDefault();
            state.AuthorizationManager = null;                  // Bypass PowerShell execution policy

            using (Runspace runspace = RunspaceFactory.CreateRunspace(host, state))
            {
                runspace.Open();

                using (Pipeline pipeline = runspace.CreatePipeline())
                {
                    Command scriptCommand = new Command(command, isScript, useLocalScope);

                    for (int i = 0; i < parameterNames.Length; i++)
                    {
                        if ( (parameterValues!=null) && ( i < parameterValues.Count ) )
                        {
                            scriptCommand.Parameters.Add(parameterNames[i], parameterValues[i]);
                        }
                        else
                        {
                            scriptCommand.Parameters.Add(parameterNames[i]);
                        }
                    }

                    scriptCommand.MergeMyResults(PipelineResultTypes.Error, PipelineResultTypes.Output);
                    pipeline.Commands.Add(scriptCommand);
                    pipeline.Commands.Add("out-default");

                    pipeline.Invoke();
                }
            }

            return ui.Output; 
        }

        class CustomPSHost : PSHost
        {
            private Guid _hostId = Guid.NewGuid();
            private CustomPSHostUserInterface _ui = new CustomPSHostUserInterface();

            public override Guid InstanceId
            {
                get { return _hostId; }
            }

            public override string Name
            {
                get { return "aardio"; }
            }

            public override Version Version
            {
                get { return new Version(3, 0); }
            }

            public override PSHostUserInterface UI
            {
                get { return _ui; }
            }


            public override CultureInfo CurrentCulture
            {
                get { return Thread.CurrentThread.CurrentCulture; }
            }

            public override CultureInfo CurrentUICulture
            {
                get { return Thread.CurrentThread.CurrentUICulture; }
            }

            public override void EnterNestedPrompt()
            {
                throw new NotImplementedException("EnterNestedPrompt is not implemented. ");
            }

            public override void ExitNestedPrompt()
            {
                throw new NotImplementedException("ExitNestedPrompt is not implemented. ");
            }

            public override void NotifyBeginApplication()
            {
                return;
            }

            public override void NotifyEndApplication()
            {
                return;
            }

            public override void SetShouldExit(int exitCode)
            {
                return;
            }
        }

        class CustomPSHostUserInterface : PSHostUserInterface
        {
            private StringBuilder _sb;
            private CustomPSRHostRawUserInterface _rawUi = new CustomPSRHostRawUserInterface();

            public CustomPSHostUserInterface()
            {
                _sb = new StringBuilder();
            }

            
            public OnWriteDelegateType OnWrite;
            public override void Write(ConsoleColor foregroundColor, ConsoleColor backgroundColor, string value)
            {
                if (OnWrite != null) OnWrite(value);
                else _sb.Append(value);
            }

            public override void WriteLine()
            {
                if (OnWrite != null) OnWrite("\n");
                else _sb.Append("\n");
            }

            public override void WriteLine(ConsoleColor foregroundColor, ConsoleColor backgroundColor, string value)
            {
                if (OnWrite != null) OnWrite(value + "\n");
                else _sb.Append(value + "\n");
            }

            public override void Write(string value)
            {
                if (OnWrite != null) OnWrite(value);
                else _sb.Append(value);
            }

            public override void WriteDebugLine(string message)
            {
                if (OnWrite != null) OnWrite("DEBUG: " + message);
                else _sb.AppendLine("DEBUG: " + message);
            }

            public override void WriteErrorLine(string value)
            {
                if (OnWrite != null) OnWrite("ERROR: " + value);
                else _sb.AppendLine("ERROR: " + value);
            }

            public override void WriteLine(string value)
            {
                if (OnWrite != null) OnWrite(value);
                else _sb.AppendLine(value);
            }

            public override void WriteVerboseLine(string message)
            {
                if (OnWrite != null) OnWrite("VERBOSE: " + message);
                else _sb.AppendLine("VERBOSE: " + message);
            }

            public override void WriteWarningLine(string message)
            {
                if (OnWrite != null) OnWrite("WARNING: " + message);
                else _sb.AppendLine("WARNING: " + message);
            }
            
            public OnWriteProgressDelegateType OnWriteProgress;
            public override void WriteProgress(long sourceId, ProgressRecord record)
            {
                if (OnWriteProgress != null) OnWriteProgress(sourceId, record); 
            }

            public string Output
            {
                get { return _sb.ToString(); }
            }

            public override Dictionary<string, PSObject> Prompt(string caption, string message, System.Collections.ObjectModel.Collection<FieldDescription> descriptions)
            {
                throw new NotImplementedException("Prompt is not implemented. ");
            }

            public override int PromptForChoice(string caption, string message, System.Collections.ObjectModel.Collection<ChoiceDescription> choices, int defaultChoice)
            {
                throw new NotImplementedException("PromptForChoice is not implemented. ");
            }

            public override PSCredential PromptForCredential(string caption, string message, string userName, string targetName, PSCredentialTypes allowedCredentialTypes, PSCredentialUIOptions options)
            {
                throw new NotImplementedException("PromptForCredential1 is not implemented. ");
            }

            public override PSCredential PromptForCredential(string caption, string message, string userName, string targetName)
            {
                throw new NotImplementedException("PromptForCredential2 is not implemented. ");
            }

            public override PSHostRawUserInterface RawUI
            {
                get { return _rawUi; }
            }
            
            public OnReadLineDelegateType OnReadLine;
            public override string ReadLine()
            {
                if (OnReadLine != null) return OnReadLine();
                throw new NotImplementedException("ReadLine is not implemented. ");
            }

            public OnReadLineDelegateType OnReadLineAsSecureString;
            public override System.Security.SecureString ReadLineAsSecureString()
            {
                /*
                if (OnReadLineAsSecureString != null) {  
                    string str = this.OnReadLineAsSecureString();
                    if (string.IsNullOrEmpty(str))
                    {
                        str = "";
                    }

                    return new System.Net.NetworkCredential("", str).SecurePassword;
                }
                */
                throw new NotImplementedException("ReadLineAsSecureString is not implemented. ");
            }
        }


        class CustomPSRHostRawUserInterface : PSHostRawUserInterface
        {
            // Warning: Setting _outputWindowSize too high will cause OutOfMemory execeptions.  I assume this will happen with other properties as well
            private Size _windowSize = new Size(120, 100);

            private Coordinates _cursorPosition = new Coordinates(0, 0);

            private int _cursorSize = 1;
            private ConsoleColor _foregroundColor = ConsoleColor.White;
            private ConsoleColor _backgroundColor = ConsoleColor.Black;

            private Size _maxPhysicalWindowSize = new Size(int.MaxValue, int.MaxValue);

            private Size _maxWindowSize = new Size(100, 100);
            private Size _bufferSize = new Size(100, 1000);
            private Coordinates _windowPosition = new Coordinates(0, 0);
            private String _windowTitle = "";

            public override ConsoleColor BackgroundColor
            {
                get { return _backgroundColor; }
                set { _backgroundColor = value; }
            }

            public override Size BufferSize
            {
                get { return _bufferSize; }
                set { _bufferSize = value; }
            }

            public override Coordinates CursorPosition
            {
                get { return _cursorPosition; }
                set { _cursorPosition = value; }
            }

            public override int CursorSize
            {
                get { return _cursorSize; }
                set { _cursorSize = value; }
            }

            public override void FlushInputBuffer()
            {
                throw new NotImplementedException("FlushInputBuffer is not implemented.");
            }

            public override ConsoleColor ForegroundColor
            {
                get { return _foregroundColor; }
                set { _foregroundColor = value; }
            }

            public override BufferCell[,] GetBufferContents(Rectangle rectangle)
            {
                throw new NotImplementedException("GetBufferContents is not implemented.");
            }

            public override bool KeyAvailable
            {
                get { throw new NotImplementedException("KeyAvailable is not implemented."); }
            }

            public override Size MaxPhysicalWindowSize
            {
                get { return _maxPhysicalWindowSize; }
            }

            public override Size MaxWindowSize
            {
                get { return _maxWindowSize; }
            }

            public override KeyInfo ReadKey(ReadKeyOptions options)
            {
                throw new NotImplementedException("ReadKey is not implemented. ");
            }

            public override void ScrollBufferContents(Rectangle source, Coordinates destination, Rectangle clip, BufferCell fill)
            {
                throw new NotImplementedException("ScrollBufferContents is not implemented");
            }

            public override void SetBufferContents(Rectangle rectangle, BufferCell fill)
            {
                throw new NotImplementedException("SetBufferContents is not implemented.");
            }

            public override void SetBufferContents(Coordinates origin, BufferCell[,] contents)
            {
                throw new NotImplementedException("SetBufferContents is not implemented");
            }

            public override Coordinates WindowPosition
            {
                get { return _windowPosition; }
                set { _windowPosition = value; }
            }

            public override Size WindowSize
            {
                get { return _windowSize; }
                set { _windowSize = value; }
            }

            public override string WindowTitle
            {
                get { return _windowTitle; }
                set { _windowTitle = value; }
            }
        }

        public static object Export(object aardioObject)
        {
            if(aardioObject!= null && aardioObject.GetType().IsCOMObject) return new DynObject(aardioObject);
            return aardioObject;
        }
    }
}