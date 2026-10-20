// Ghidra headless script: dump the raw disassembly of one or more functions.
//
//   -postScript DumpDisasm.java <outDir> <addr|fn:Name> [...]
//
// The decompiler (DumpDecomp.java) loses register-level detail around the
// helper calls of the official aardio record interpreter (FUN_004C4BC0):
// the operand roles of the get/set member variants cannot be recovered from
// the C output alone.  This script prints the instruction listing so the
// register setup in front of every call can be read directly.
//
// Writes disasm_<function>.txt into <outDir>.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class DumpDisasm extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpDisasm <outDir> <addr|fn:Name> [...]");
            return;
        }
        String outDir = args[0];
        new File(outDir).mkdirs();

        List<Address> targets = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            String spec = args[i];
            Address a = toAddr(spec.startsWith("fn:") ? spec.substring(3) : spec);
            Function f = getFunctionAt(a);
            if (f == null) f = getFunctionContaining(a);
            if (f == null) {
                println("no function at " + spec);
                continue;
            }
            targets.add(f.getEntryPoint());
        }

        for (Address entry : targets) {
            Function f = getFunctionAt(entry);
            if (f == null) continue;
            File out = new File(outDir, "disasm_" + f.getName() + ".txt");
            PrintWriter w = new PrintWriter(out, "UTF-8");
            w.println("// " + f.getName() + " @ " + f.getEntryPoint() +
                      " size=" + f.getBody().getNumAddresses());
            InstructionIterator it = currentProgram.getListing()
                .getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                w.println(ins.getAddress().toString() + "  " + ins.toString());
            }
            w.close();
            println("wrote " + out.getPath());
        }
    }
}
