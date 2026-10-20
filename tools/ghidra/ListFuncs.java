// Ghidra headless script: list function names that are not Ghidra's
// synthetic FUN_###### placeholders.
//
//   -postScript ListFuncs.java <outDir> [nameFilter]
//
// The official aardio.exe keeps some of its original C++ symbol names in
// RTTI / export tables, so the non-synthetic names are the fastest way to
// locate the VM components (chunk loader, opcode interpreter, ...).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.File;
import java.io.PrintWriter;

public class ListFuncs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: ListFuncs <outDir> [nameFilter]");
            return;
        }
        String outDir = args[0];
        String filter = (args.length > 1) ? args[1].toLowerCase() : null;
        new File(outDir).mkdirs();
        PrintWriter pw = new PrintWriter(new File(outDir, "funcs.txt"));

        int total = 0;
        int named = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            total++;
            String name = f.getName();
            if (name.startsWith("FUN_") || name.startsWith("thunk_") || name.startsWith("LAB_")
                || name.startsWith("SUB_") || name.startsWith("DAT_")) {
                continue;
            }
            if (filter != null && !name.toLowerCase().contains(filter)) {
                continue;
            }
            named++;
            String line = name + " @ " + f.getEntryPoint() + "  size=" + f.getBody().getNumAddresses();
            pw.println(line);
        }
        pw.println("// total functions: " + total + ", non-synthetic listed: " + named);
        pw.close();
        println("// total functions: " + total + ", non-synthetic listed: " + named);
    }
}
