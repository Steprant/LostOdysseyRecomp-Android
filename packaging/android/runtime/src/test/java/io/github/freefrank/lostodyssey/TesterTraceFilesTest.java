package io.github.freefrank.lostodyssey;

import static org.junit.Assert.*;
import org.junit.Test;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class TesterTraceFilesTest {
    @Test public void exportsSlowWindowAndPercentiles() throws Exception {
        File dir = Files.createTempDirectory("lo-trace").toFile();
        TesterTraceFiles writer = new TesterTraceFiles(dir);
        for (int i=1; i<=50; ++i) {
            writer.appendFrames(i * 1_000_000_000L + ",1000000000,1,1,1,2,3,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1," + (i == 35 ? 1 : 0) + "\n", true);
        }
        writer.close();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        TesterTraceFiles.export(dir, Collections.emptyList(), bytes);
        Map<String,String> files = new HashMap<>();
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;)
                files.put(entry.getName(), new String(zip.readAllBytes(), StandardCharsets.UTF_8));
        }
        assertTrue(files.get("summary.json").contains("\"p95_interval_ns\":1000000000"));
        assertTrue(files.get("summary.json").contains("\"estimated_1pct_low_accepted_present_fps\":1.000"));
        assertTrue(files.get("events/event-1.csv").contains("\n5000000000,"));
        assertTrue(files.get("events/event-1.csv").contains("\n45000000000,"));
        assertFalse(files.get("events/event-1.csv").contains("\n46000000000,"));
        assertTrue(files.containsKey("gpu.csv"));
    }
    @Test public void readsThreadStatWithParentheses() {
        TesterThreadStats stat = TesterThreadStats.parse("123 (Render (main)) R 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20");
        assertNotNull(stat);
        assertEquals("Render (main)", stat.name);
        assertEquals(23, stat.ticks);
        assertEquals(19, stat.startTicks);
        assertNull(TesterThreadStats.parse("not a stat"));
    }

    @Test public void reportsNativeAndGeneratedPresentsSeparately() throws Exception {
        File dir = Files.createTempDirectory("lo-fg-trace").toFile();
        TesterTraceFiles writer = new TesterTraceFiles(dir);
        for (int i = 1; i <= 4; ++i) {
            String[] row = new String[29];
            java.util.Arrays.fill(row, "0");
            row[0] = Long.toString(i * 16666667L);
            row[1] = "16666667";
            row[22] = "1";
            row[26] = i % 2 == 0 ? "1" : "0";
            row[27] = Integer.toString(i);
            row[28] = row[0];
            writer.appendFrames(String.join(",", row) + "\n", true);
        }
        writer.close();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        TesterTraceFiles.export(dir, Collections.emptyList(), bytes);
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                if (!entry.getName().equals("summary.json")) continue;
                String summary = new String(zip.readAllBytes(), StandardCharsets.UTF_8);
                assertTrue(summary.contains("\"native_presents_per_second\":30.000"));
                assertTrue(summary.contains("\"generated_presents_per_second\":30.000"));
                return;
            }
        }
        fail("Missing summary.json");
    }
}
