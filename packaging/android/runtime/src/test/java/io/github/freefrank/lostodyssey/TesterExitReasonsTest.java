package io.github.freefrank.lostodyssey;

import static org.junit.Assert.*;
import org.junit.Test;
import java.util.Arrays;

public class TesterExitReasonsTest {
    @Test public void namesTheAndroidReasons() {
        assertEquals("low_memory", TesterExitReasons.reasonName(3));
        assertEquals("crash_native", TesterExitReasons.reasonName(5));
        assertEquals("anr", TesterExitReasons.reasonName(6));
        assertEquals("unknown", TesterExitReasons.reasonName(99));
        assertTrue(TesterExitReasons.crash("crash_native"));
        assertFalse(TesterExitReasons.crash("low_memory"));
    }
    @Test public void writesExitsAsJson() {
        TesterExitReasons.Exit crash = new TesterExitReasons.Exit();
        crash.timestampMs = 1700000000000L; crash.pid = 42; crash.reason = "crash_native"; crash.status = 11;
        crash.importance = 100; crash.pssKb = 3_400_000; crash.rssKb = 3_600_000;
        crash.description = "signal 11 \"SEGV\" in /data/user/0/io.example/lib.so";
        TesterExitReasons.Exit kill = new TesterExitReasons.Exit();
        kill.reason = "low_memory";
        String json = TesterExitReasons.json(Arrays.asList(crash, kill), null);
        assertTrue(json.startsWith("{\"available\":true,\"exits\":[{\"timestamp_ms\":1700000000000,\"pid\":42,\"reason\":\"crash_native\""));
        assertTrue(json.contains("\"pss_kb\":3400000"));
        assertTrue(json.contains("signal 11 \\\"SEGV\\\" in [app-private]"));
        assertTrue(json.contains("{\"timestamp_ms\":0,\"pid\":0,\"reason\":\"low_memory\""));
        assertEquals("exits/exit-1700000000000-crash_native.tombstone.pb", TesterExitReasons.traceName(crash));
        assertEquals("{\"available\":false,\"reason\":\"Android 11 or later is needed\"}\n",
            TesterExitReasons.json(null, "Android 11 or later is needed"));
    }
}
