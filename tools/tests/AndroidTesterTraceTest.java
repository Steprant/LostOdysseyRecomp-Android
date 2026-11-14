package io.github.freefrank.lostodyssey;

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

/** Standalone JVM test; no Android SDK or emulator. */
public final class AndroidTesterTraceTest {
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) throws Exception {
        TesterThreadStats parsed = TesterThreadStats.parse("123 (Render (main)) R 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20");
        check(parsed != null && parsed.name.equals("Render (main)") && parsed.ticks == 23 && parsed.startTicks == 19, "thread stat parser");
        check(TesterThreadStats.parse("123 malformed") == null, "unavailable thread stat");
        long[] sched = TesterThreadStats.parseSchedstat("5000000 2500000 42\n");
        check(sched != null && sched[0] == 5000000 && sched[1] == 2500000 && sched[2] == 42, "schedstat parser");
        check(TesterThreadStats.parseSchedstat("12 x") == null && TesterThreadStats.parseSchedstat(null) == null, "unreadable schedstat");
        // Steprant 2.1.1: a slow interval is the 24th column, whatever follows it.
        check(TesterTraceFiles.slowInterval("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,1,1"), "old 24-column line");
        check(TesterTraceFiles.slowInterval("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,1,1,0,0"), "slow, not paused");
        check(!TesterTraceFiles.slowInterval("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,1,0,1,0"), "a paused interval is not a slow one");
        check(!TesterTraceFiles.slowInterval("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,1,0,0,1"), "nor is a profiled frame");
        check(!TesterTraceFiles.slowInterval("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,1,1,1,0"), "slow but paused: left out");
        check(TesterTraceFiles.inProfilingBurst(1000L + 48_000_000_000L, 1000L) && !TesterTraceFiles.inProfilingBurst(1000L + 47_000_000_000L, 1000L)
            && !TesterTraceFiles.inProfilingBurst(5, 0), "profiling bursts: last 12 s of each minute, none without a start");
        File dir = Files.createTempDirectory("lo-tester").toFile();
        TesterTraceFiles writer = new TesterTraceFiles(dir);
        TesterTraceFiles.writeText(new File(dir, "device.json"), "{}\n");
        TesterTraceFiles.writeText(new File(dir, "session.json"), "{\n  \"started_ns\": 1000000000,\n  \"x\": 1\n}\n");
        for (int i = 1; i <= 50; ++i) {
            long time = i * 1_000_000_000L;
            // 2.1.1 format: ...,slow_interval,game_paused,profiling; frames 49-50 are in the burst.
            writer.appendFrames(time + ",1000000000,1,1,1,2,3,4," + (i >= 40 ? 3 : 0) + ",0,0,0,0,0,0,0,0,0,0,0,0,0,1," + (i == 35 ? 1 : 0)
                + ",0," + (i >= 49 ? 1 : 0) + "\n", true);
        }
        writer.appendCpu("1,2,,4\n"); // the first sample has no percentage
        writer.appendCpu("2000000000,10,150.00,8\n");
        writer.appendCpu("50000000000,10,400.00,8\n");
        writer.appendThermal("1,2,,35.0\n");
        // Unplugged: 1 A at 4000 mV = 4 W; the burst sample (8 W) and the
        // charger sample stay out of the profiling-free mean.
        writer.appendThermal("2000000000,0,,36.0,1000000,4000,0\n");
        writer.appendThermal("50000000000,0,,37.0,-2000000,4000,0\n");
        writer.appendThermal("51000000000,1,,38.0,1500000,4000,2\n");
        writer.appendFrequencies("2000000000,0,1200000\n2000000000,7,2100000\n3000000000,7,600000\n3000000000,0,500000\n");
        writer.appendSystem("2000000000,60.00,60.00,512,0,0,1,80\n");
        writer.close();
        File log = new File(dir, "runtime-123.log");
        Files.writeString(log.toPath(), "path /storage/emulated/0/private/file.txt\n");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        TesterTraceFiles.export(dir, Collections.singletonList(log), bytes);
        Map<String,String> files = new HashMap<>();
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;)
                files.put(entry.getName(), new String(zip.readAllBytes(), StandardCharsets.UTF_8));
        }
        check(files.get("frame_timing.csv").startsWith(TesterTraceFiles.FRAME_HEADER), "frame header");
        check(files.get("summary.json").contains("\"p95_interval_ns\":1000000000"), "p95");
        check(files.get("summary.json").contains("\"estimated_1pct_low_accepted_present_fps\":1.000"), "1% low accepted presents");
        String window = files.get("events/event-1.csv");
        check(window != null && window.contains("\n5000000000,") && window.contains("\n45000000000,"), "before/after window");
        check(!window.contains("\n4000000000,") && !window.contains("\n46000000000,"), "bounded event window");
        check(!files.get("runtime.log").contains("/storage/emulated/0/"), "redact shared path");
        check(files.get("thermal.csv").contains("1,2,,35.0"), "unavailable thermal data remains empty");
        check(files.get("system.csv").startsWith(TesterTraceFiles.SYSTEM_HEADER) && files.get("system.csv").contains("60.00,60.00,512"), "system.csv");
        String summary = files.get("summary.json");
        check(summary.contains("\"profiled_intervals\":2"), "profiled frames counted: " + summary);
        check(summary.contains("\"audio_queue_empty_during_recording\":3"), "audio empties during the recording");
        check(summary.contains("\"mean_w_outside_profiling\":4.00") && summary.contains("\"mean_w_all_samples\":6.00"), "power outside bursts");
        check(summary.contains("\"samples_on_charger\":1") && summary.contains("\"battery_c_max\":38.0"), "charger samples and temperature");
        check(summary.contains("\"android_thermal_status_max\":2"), "thermal status");
        check(summary.contains("\"process_cpu_percent_one_core_mean_outside_profiling\":150.0"), "CPU outside bursts");
        check(summary.contains("\"fastest_core_khz_lowest_sample\":600000"), "lowest fastest-core frequency");
        check(!files.containsKey("previous-runtime.log"), "no previous run with one log");

        // SR4 S-2: after a restart the run before the newest one is the one
        // that may have crashed; its tail goes in as previous-runtime.log, and
        // the caller's extra entries (exit reasons) are written into the ZIP.
        File older = new File(dir, "runtime-122.log");
        Files.writeString(older.toPath(), "crashed run /data/user/0/io.github.example/files/x\n");
        bytes = new ByteArrayOutputStream();
        TesterTraceFiles.export(dir, java.util.Arrays.asList(older, log), zip -> {
            zip.putNextEntry(new ZipEntry("exit-reasons.json"));
            zip.write("{\"available\":true}\n".getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
        }, bytes);
        files.clear();
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;)
                files.put(entry.getName(), new String(zip.readAllBytes(), StandardCharsets.UTF_8));
        }
        check(files.get("runtime.log").contains("path [shared-path]"), "newest log stays runtime.log");
        check(files.get("previous-runtime.log") != null && files.get("previous-runtime.log").contains("crashed run [app-private]"),
            "previous run's log, redacted");
        check("{\"available\":true}\n".equals(files.get("exit-reasons.json")), "extra entries");
        // SR4 C-4: the profiles of an instrumented build, and nothing else.
        File pgo = Files.createTempDirectory("lo-pgo").toFile();
        Files.write(new File(pgo, "lostodyssey-42.profraw").toPath(), new byte[]{1, 2, 3});
        Files.writeString(new File(pgo, "notes.txt").toPath(), "x");
        bytes = new ByteArrayOutputStream();
        TesterTraceFiles.export(dir, Collections.emptyList(), zip -> TesterTraceFiles.zipFiles(zip, pgo, ".profraw", "pgo/"), bytes);
        files.clear();
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;)
                files.put(entry.getName(), new String(zip.readAllBytes(), StandardCharsets.ISO_8859_1));
        }
        check("\u0001\u0002\u0003".equals(files.get("pgo/lostodyssey-42.profraw")) && !files.containsKey("pgo/notes.txt"), "profiles only");
        TesterTraceFiles.export(dir, Collections.emptyList(), zip -> TesterTraceFiles.zipFiles(zip, new File(pgo, "missing"), ".profraw", "pgo/"),
            new ByteArrayOutputStream());
        bytes = new ByteArrayOutputStream();
        TesterTraceFiles.export(dir, java.util.Arrays.asList(older, log), zip -> { throw new java.io.IOException("no service"); }, bytes);
        files.clear();
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;)
                files.put(entry.getName(), new String(zip.readAllBytes(), StandardCharsets.UTF_8));
        }
        check(files.get("exit-reasons.json").contains("\"available\":false"), "a failing extra does not break the export");
        // Steprant 2.0.2: a frame capture or a profile from an older run of the
        // app stays out of the export; one from this run goes in.
        File root = Files.createTempDirectory("lo-diagnostics").toFile();
        File session = new File(root, "session-7");
        new TesterTraceFiles(session).close();
        File samples = new File(root, "frame-samples");
        check(samples.mkdirs(), "frame-samples directory");
        File oldSample = new File(samples, "frame-01-swap8463-presented.png");
        File newSample = new File(samples, "frame-01-swap9000-presented.png");
        Files.write(oldSample.toPath(), new byte[]{7});
        Files.write(newSample.toPath(), new byte[]{8});
        File oldProfile = new File(pgo, "lostodyssey-21645.profraw");
        Files.write(oldProfile.toPath(), new byte[]{9});
        long start = System.currentTimeMillis() - 1000;
        check(oldSample.setLastModified(start - 86_400_000L) && oldProfile.setLastModified(start - 86_400_000L), "old file times");
        check(newSample.setLastModified(start + 500) && new File(pgo, "lostodyssey-42.profraw").setLastModified(start + 500), "new file times");
        bytes = new ByteArrayOutputStream();
        TesterTraceFiles.export(session, Collections.emptyList(),
            zip -> TesterTraceFiles.zipFiles(zip, pgo, ".profraw", "pgo/", start), start, bytes);
        files.clear();
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;)
                files.put(entry.getName(), new String(zip.readAllBytes(), StandardCharsets.ISO_8859_1));
        }
        check(files.containsKey("frame-samples/frame-01-swap9000-presented.png"), "capture of this run exported");
        check(!files.containsKey("frame-samples/frame-01-swap8463-presented.png"), "capture of an older run left out");
        check(files.containsKey("pgo/lostodyssey-42.profraw") && !files.containsKey("pgo/lostodyssey-21645.profraw"), "only this run's profile");
        check(files.get("summary.json").contains("\"frame_samples_included\":1,\"frame_samples_skipped_older_run\":1"), "capture counts in the summary");
        System.out.println("PASS: Android tester trace export and event window (JVM)");
    }
}
