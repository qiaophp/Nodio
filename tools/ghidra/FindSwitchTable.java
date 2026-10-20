// Ghidra headless script: find switch/jump tables of N consecutive code
// pointers inside the program's data sections.
//
//   -postScript FindSwitchTable.java <outDir> <count>
//
// Scans every aligned 4-byte slot of the initialized, non-executable memory
// blocks; when N consecutive dwords all point into an executable block, the
// run is reported as a candidate dispatch table. Used to locate the official
// aardio.exe opcode dispatch table (64 entries).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

import java.io.File;
import java.io.PrintWriter;

public class FindSwitchTable extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: FindSwitchTable <outDir> <count>");
            return;
        }
        String outDir = args[0];
        int need = Integer.parseInt(args[1].trim());
        new File(outDir).mkdirs();
        PrintWriter pw = new PrintWriter(new File(outDir, "switchtables.txt"));

        var mem = currentProgram.getMemory();

        for (var blk : mem.getBlocks()) {
            if (!blk.isInitialized() || !blk.isRead() || blk.isExecute()) continue;
            long start = blk.getStart().getOffset();
            long end = blk.getEnd().getOffset();
            long p = start;
            while (p + (long) need * 4 <= end) {
                int ok = 0;
                long q = p;
                while (ok < need) {
                    int v;
                    try {
                        v = mem.getInt(toAddr(q));
                    } catch (Exception e) {
                        break;
                    }
                    if (v == 0) break;
                    var codeBlk = mem.getBlock(toAddr(v & 0xffffffffL));
                    if (codeBlk == null || !codeBlk.isExecute()) break;
                    ok++;
                    q += 4;
                }
                if (ok == need) {
                    String line = "TABLE at " + toAddr(p) + " count=" + need;
                    println(line);
                    pw.println(line);
                    for (int k = 0; k < need; k++) {
                        int v = mem.getInt(toAddr(p + (long) k * 4));
                        pw.println("    [" + k + "] " + toAddr(v & 0xffffffffL));
                    }
                    p += (long) need * 4;
                    continue;
                }
                p += 4;
            }
        }
        pw.close();
    }
}
