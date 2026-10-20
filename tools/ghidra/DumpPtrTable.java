// Ghidra headless script: dump a 32-bit pointer table and resolve each entry.
//
//   -postScript DumpPtrTable.java <outDir> <addr:count> [...]
//
// For every u32 entry it prints the raw value plus the symbol it resolves to
// (function entry, function containing it, or a data symbol). Used to map
// the official aardio.exe dispatch tables (opcode handlers, node handlers).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;

import java.io.File;
import java.io.PrintWriter;

public class DumpPtrTable extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpPtrTable <outDir> <addr:count> [...]");
            return;
        }
        String outDir = args[0];
        new File(outDir).mkdirs();
        PrintWriter pw = new PrintWriter(new File(outDir, "ptrtable.txt"));

        for (int i = 1; i < args.length; i++) {
            String spec = args[i];
            int colon = spec.lastIndexOf(':');
            Address base = toAddr(spec.substring(0, colon));
            int count = Integer.parseInt(spec.substring(colon + 1).trim());
            println("=== ptr table " + spec + " ===");
            pw.println("=== ptr table " + spec + " ===");

            for (int k = 0; k < count; k++) {
                Address slot = base.add(k * 4L);
                Integer v = null;
                try {
                    v = (int) currentProgram.getMemory().getInt(slot);
                } catch (Exception e) {
                    // fall through
                }
                String desc = "?";
                if (v != null) {
                    Address target = toAddr(v & 0xffffffffL);
                    Function f = getFunctionAt(target);
                    if (f != null) {
                        desc = "FUNC " + f.getName();
                    } else {
                        f = getFunctionContaining(target);
                        if (f != null) {
                            desc = "FUNC+0x" + Long.toHexString(target.subtract(f.getEntryPoint()));
                        } else {
                            Symbol s = getSymbolAt(target);
                            if (s != null) {
                                desc = "SYM " + s.getName();
                            } else {
                                desc = "data?";
                            }
                        }
                    }
                    String line = String.format("[%02d] slot=%08x value=%08x  %s",
                        k, slot.getOffset(), v & 0xffffffffL, desc);
                    println(line);
                    pw.println(line);
                }
            }
        }
        pw.close();
    }
}
