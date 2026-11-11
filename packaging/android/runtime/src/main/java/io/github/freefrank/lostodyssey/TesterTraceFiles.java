package io.github.freefrank.lostodyssey;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Bounded, private trace files. All methods run on the collector thread. */
final class TesterTraceFiles {
    static final String FRAME_HEADER = "monotonic_ns,interval_ns,present_wall_ns,present_thread_cpu_ns,renderer_frame,draws,gpu_completed_batch_total_ns,gpu_completed_batches,audio_queue_empty_count,audio_clock_resets,xma_decode_attempts,xma_decode_failures,xma_decode_wall_total_ns,movie_guest_frames,movie_guest_pts_ms,movie_update_wall_ns,inline_pipeline_attempts,inline_pipeline_failures,inline_pipeline_wall_total_ns,last_pipeline_vs_hash,last_pipeline_ps_hash,movie_active,present_accepted,slow_interval,game_paused\n";
    static final String CPU_HEADER = "monotonic_ns,process_cpu_time_ms,process_cpu_percent_one_core,logical_processors\n";
    static final String THERMAL_HEADER = "monotonic_ns,android_thermal_status,thermal_headroom,battery_temperature_c,battery_current_ua,battery_voltage_mv,battery_plugged\n";
    static final String THREAD_HEADER = "monotonic_ns,thread_id,name,state,thread_cpu_ticks,ticks_per_second,thread_cpu_percent_one_core\n";
    static final String FREQUENCY_HEADER = "monotonic_ns,cpu_core,scaling_frequency_khz\n";
    static final String MEMORY_HEADER = "monotonic_ns,process_pss_kb,native_heap_allocated_bytes\n";
    private static final long SEGMENT_SIZE = 2L * 1024 * 1024;
    private static final long EVENT_MAX_SIZE = 4L * 1024 * 1024;
    private static final int SEGMENTS = 4, EVENT_LIMIT = 3, RECENT_LIMIT = 16384;
    private static final long BEFORE_NS = 30_000_000_000L, AFTER_NS = 10_000_000_000L;
    private final File directory;
    private final Rolling frames, cpu, thermal, threads, frequencies, memory;
    private final ArrayDeque<Line> recent = new ArrayDeque<>();
    private FileOutputStream event;
    private long eventEnd, eventBytes;
    private int eventCount;

    private static final class Line {
        final long time;
        final String text;
        Line(long time, String text) { this.time = time; this.text = text; }
    }
    private static final class Rolling {
        final File directory;
        final String prefix;
        int number;
        FileOutputStream stream;
        long size;
        Rolling(File directory, String prefix) throws IOException {
            this.directory = directory; this.prefix = prefix; rotate();
        }
        void rotate() throws IOException {
            if (stream != null) stream.close();
            File file = new File(directory, String.format(java.util.Locale.US, "%s-%06d.csv", prefix, ++number));
            stream = new FileOutputStream(file); size = 0;
            File[] files = files(directory, prefix);
            for (int i = 0; i < files.length - SEGMENTS; ++i)
                if (!files[i].delete()) throw new IOException("cannot remove old trace segment");
        }
        void append(String text) throws IOException {
            byte[] data = text.getBytes(StandardCharsets.UTF_8);
            if (size > 0 && size + data.length > SEGMENT_SIZE) rotate();
            stream.write(data); size += data.length;
        }
        void flush() throws IOException { stream.flush(); }
        void close() throws IOException { if (stream != null) { stream.close(); stream = null; } }
    }
    TesterTraceFiles(File directory) throws IOException {
        this.directory = directory;
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("cannot create trace directory");
        Rolling f = null, c = null, t = null, th = null, freq = null, mem = null;
        try {
            f = new Rolling(directory, "frames"); c = new Rolling(directory, "cpu");
            t = new Rolling(directory, "thermal"); th = new Rolling(directory, "threads");
            freq = new Rolling(directory, "frequencies");
            mem = new Rolling(directory, "memory");
        } catch (IOException error) {
            for (Rolling stream : new Rolling[]{f, c, t, th, freq, mem})
                if (stream != null) try { stream.close(); } catch (IOException closeError) { error.addSuppressed(closeError); }
            throw error;
        }
        frames = f; cpu = c; thermal = t; threads = th; frequencies = freq; memory = mem;
    }
    private static File[] files(File directory, String prefix) {
        File[] list = directory.listFiles((d, name) -> name.startsWith(prefix + "-") && name.endsWith(".csv"));
        if (list == null) return new File[0];
        Arrays.sort(list, Comparator.comparing(File::getName));
        return list;
    }
    void appendFrames(String batch, boolean captureEvents) throws IOException {
        if (batch == null || batch.isEmpty()) return;
        for (String value : batch.split("\n")) {
            if (value.isEmpty()) continue;
            int comma = value.indexOf(',');
            if (comma < 0) continue;
            long now;
            try { now = Long.parseLong(value.substring(0, comma)); }
            catch (NumberFormatException bad) { continue; }
            String line = value + "\n";
            frames.append(line);
            while (!recent.isEmpty() && (recent.peekFirst().time < now - BEFORE_NS || recent.size() >= RECENT_LIMIT))
                recent.removeFirst();
            recent.addLast(new Line(now, line));
            if (event != null) {
                appendEvent(line);
                if (event != null && now >= eventEnd) { event.close(); event = null; }
            } else if (captureEvents && value.endsWith(",1") && eventCount < EVENT_LIMIT) {
                event = new FileOutputStream(new File(directory, "event-" + (++eventCount) + ".csv"));
                eventBytes = 0;
                appendEvent(FRAME_HEADER);
                for (Line previous : recent) { if (event == null) break; appendEvent(previous.text); }
                eventEnd = now + AFTER_NS;
            }
        }
        frames.flush();
        if (event != null) event.flush();
    }
    private void appendEvent(String line) throws IOException {
        byte[] bytes = line.getBytes(StandardCharsets.UTF_8);
        if (eventBytes + bytes.length > EVENT_MAX_SIZE) { event.close(); event = null; return; }
        event.write(bytes); eventBytes += bytes.length;
    }
    void appendCpu(String line) throws IOException { cpu.append(line); cpu.flush(); }
    void appendThermal(String line) throws IOException { thermal.append(line); thermal.flush(); }
    void appendThreads(String lines) throws IOException { if (!lines.isEmpty()) { threads.append(lines); threads.flush(); } }
    void appendFrequencies(String lines) throws IOException { if (!lines.isEmpty()) { frequencies.append(lines); frequencies.flush(); } }
    void appendMemory(String line) throws IOException { memory.append(line); memory.flush(); }
    void close() throws IOException {
        IOException failure = null;
        if (event != null) { try { event.close(); } catch (IOException error) { failure = error; } event = null; }
        for (Rolling stream : new Rolling[]{frames, cpu, thermal, threads, frequencies, memory}) {
            try { stream.close(); } catch (IOException error) {
                if (failure == null) failure = error; else failure.addSuppressed(error);
            }
        }
        if (failure != null) throw failure;
    }
    static void writeText(File file, String value) throws IOException {
        File temporary = new File(file.getParentFile(), file.getName() + ".tmp");
        try {
            try (FileOutputStream out = new FileOutputStream(temporary)) { out.write(value.getBytes(StandardCharsets.UTF_8)); }
            try {
                java.nio.file.Files.move(temporary.toPath(), file.toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE);
            } catch (java.nio.file.AtomicMoveNotSupportedException unsupported) {
                java.nio.file.Files.move(temporary.toPath(), file.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
        } finally { if (temporary.exists()) temporary.delete(); }
    }
    private static void zipText(ZipOutputStream zip, String name, String text) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(text.getBytes(StandardCharsets.UTF_8));
        zip.closeEntry();
    }
    private static void copy(ZipOutputStream zip, File file) throws IOException {
        try (BufferedInputStream in = new BufferedInputStream(new FileInputStream(file))) {
            byte[] buffer = new byte[16384];
            for (int n; (n = in.read(buffer)) != -1;) zip.write(buffer, 0, n);
        }
    }
    // SR4 C-4: the files of a directory ending in suffix, newest first, as
    // prefix + name (the profiles of an instrumented build).
    static void zipFiles(ZipOutputStream zip, File dir, String suffix, String prefix) throws IOException {
        zipFiles(zip, dir, suffix, prefix, Long.MIN_VALUE);
    }
    // Steprant 2.0.2: only files written since sinceMs (wall clock, as
    // File.lastModified), so a profile left over from an older run or build
    // is no longer attached to every export.
    static void zipFiles(ZipOutputStream zip, File dir, String suffix, String prefix, long sinceMs) throws IOException {
        File[] found = dir == null ? null : dir.listFiles((d, name) -> name.endsWith(suffix));
        if (found == null) return;
        java.util.Arrays.sort(found, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        for (File f : found) {
            if (!f.isFile() || f.lastModified() < sinceMs) continue;
            zip.putNextEntry(new ZipEntry(prefix + f.getName()));
            copy(zip, f);
            zip.closeEntry();
        }
    }
    private static void zipSegments(ZipOutputStream zip, File dir, String prefix, String name, String header) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(header.getBytes(StandardCharsets.UTF_8));
        for (File f : files(dir, prefix)) copy(zip, f);
        zip.closeEntry();
    }
    private static void zipGpu(ZipOutputStream zip, File dir) throws IOException {
        zip.putNextEntry(new ZipEntry("gpu.csv"));
        zip.write("monotonic_ns,completed_batch_total_ns,completed_batch_count,completed_batch_interval_ns,completed_batch_interval_count\n".getBytes(StandardCharsets.UTF_8));
        long previousTime = 0, previousTotal = 0, previousCount = 0;
        for (File file : files(dir, "frames")) {
            try (BufferedReader in = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
                for (String line; (line = in.readLine()) != null;) {
                    String[] values = line.split(",", -1);
                    if (values.length < 24) continue;
                    try {
                        long time = Long.parseLong(values[0]), total = Long.parseLong(values[6]), count = Long.parseLong(values[7]);
                        if (previousTime != 0 && time - previousTime < 1_000_000_000L) continue;
                        String delta = previousTime != 0 && count > previousCount && total >= previousTotal
                            ? (total - previousTotal) + "," + (count - previousCount) : ",";
                        String row = time + "," + total + "," + count + "," + delta + "\n";
                        zip.write(row.getBytes(StandardCharsets.UTF_8));
                        previousTime = time; previousTotal = total; previousCount = count;
                    } catch (NumberFormatException ignored) { }
                }
            }
        }
        zip.closeEntry();
    }
    // Extra entries written into the Tester ZIP by the caller (SR4 S-2: exit
    // reasons, which need an Android Context).
    interface Extras { void write(ZipOutputStream zip) throws IOException; }

    static String redactPaths(String text) {
        return text.replaceAll("/data/user/[0-9]+/[^\\s]+", "[app-private]")
            .replaceAll("/data/(?:user_de|data)/[^\\s]+", "[app-private]")
            .replaceAll("/storage/emulated/[0-9]+/[^\\s]+", "[shared-path]")
            .replaceAll("luid=[^\\s]+", "luid=[redacted]");
    }

    // The last `limit` bytes of a log, with private paths redacted.
    static String logTail(File log, long limit) throws IOException {
        try (java.io.RandomAccessFile in = new java.io.RandomAccessFile(log, "r")) {
            long end = in.length();
            long start = Math.max(0, end - limit);
            in.seek(start);
            long remaining = end - start;
            ByteArrayOutputStream tail = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            while (remaining > 0) {
                int n = in.read(buffer, 0, (int) Math.min(buffer.length, remaining));
                if (n <= 0) break;
                tail.write(buffer, 0, n); remaining -= n;
            }
            return redactPaths(new String(tail.toByteArray(), StandardCharsets.UTF_8));
        }
    }

    static void export(File dir, List<File> logs, OutputStream output) throws IOException {
        export(dir, logs, null, output);
    }

    static void export(File dir, List<File> logs, Extras extras, OutputStream output) throws IOException {
        export(dir, logs, extras, Long.MIN_VALUE, output);
    }

    // samplesSinceMs (Steprant 2.0.2): frame-samples/ files written before it
    // (wall clock, as File.lastModified) belong to an older run of the app and
    // stay out of the ZIP. The Tester passes the start of the app's process.
    static void export(File dir, List<File> logs, Extras extras, long samplesSinceMs, OutputStream output) throws IOException {
        ZipOutputStream zip = new ZipOutputStream(output);
        for (String name : new String[]{"device.json", "session.json"}) {
            File file = new File(dir, name);
            if (file.isFile()) { zip.putNextEntry(new ZipEntry(name)); copy(zip, file); zip.closeEntry(); }
        }
        zipSegments(zip, dir, "frames", "frame_timing.csv", FRAME_HEADER);
        zipGpu(zip, dir);
        zipSegments(zip, dir, "cpu", "cpu.csv", CPU_HEADER);
        zipSegments(zip, dir, "thermal", "thermal.csv", THERMAL_HEADER);
        zipSegments(zip, dir, "threads", "threads.csv", THREAD_HEADER);
        zipSegments(zip, dir, "frequencies", "cpu_frequencies.csv", FREQUENCY_HEADER);
        zipSegments(zip, dir, "memory", "memory.csv", MEMORY_HEADER);
        for (File f : files(dir, "event")) { zip.putNextEntry(new ZipEntry("events/" + f.getName())); copy(zip, f); zip.closeEntry(); }
        // Steprant 2.0 WP0.1: bit-exact frame samples from the developer page
        // (android-diagnostics/frame-samples, beside the sessions). 2.0.2: only
        // a capture of this run of the app; one from before stays on the phone
        // until "Clear diagnostics" or the next capture removes it.
        File samples = dir.getParentFile() == null ? null : new File(dir.getParentFile(), "frame-samples");
        File[] sampleFiles = samples == null ? null : samples.listFiles();
        int samplesIncluded = 0, samplesSkipped = 0;
        if (sampleFiles != null) {
            java.util.Arrays.sort(sampleFiles);
            for (File f : sampleFiles) {
                if (!f.isFile()) continue;
                if (f.lastModified() < samplesSinceMs) { ++samplesSkipped; continue; }
                ++samplesIncluded;
                zip.putNextEntry(new ZipEntry("frame-samples/" + f.getName()));
                copy(zip, f);
                zip.closeEntry();
            }
        }
        List<Long> intervals = new ArrayList<>();
        long totalInterval = 0;
        int spikes = 0, pausedIntervals = 0;
        StringBuilder spikeTimes = new StringBuilder();
        for (File file : files(dir, "frames")) {
            try (BufferedReader in = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
                for (String line; (line = in.readLine()) != null;) {
                    String[] fields = line.split(",", -1);
                    if (fields.length < 24) continue;
                    try {
                        // Steprant 2.0: an interval that spans a pause of the
                        // game (overlay, background) is not a frame.
                        if (fields.length > 24 && fields[24].equals("1")) { ++pausedIntervals; continue; }
                        long value = Long.parseLong(fields[1]);
                        if (value > 0) { intervals.add(value); totalInterval += value; }
                        if (fields[23].equals("1")) {
                            ++spikes;
                            if (spikes <= 1000) {
                                if (spikeTimes.length() != 0) spikeTimes.append(',');
                                spikeTimes.append(Long.parseLong(fields[0]));
                            }
                        }
                    } catch (NumberFormatException ignored) { }
                }
            }
        }
        Collections.sort(intervals);
        long p95 = percentile(intervals, 95), p99 = percentile(intervals, 99);
        String fps = totalInterval > 0 ? String.format(java.util.Locale.US, "%.3f", intervals.size() * 1_000_000_000.0 / totalInterval) : "null";
        int slowestOnePercent = (intervals.size() + 99) / 100;
        double slowestTime = 0;
        for (int i = intervals.size() - slowestOnePercent; i < intervals.size(); ++i)
            slowestTime += intervals.get(i);
        String onePercentLow = slowestTime > 0 ? String.format(java.util.Locale.US, "%.3f", slowestOnePercent * 1_000_000_000.0 / slowestTime) : "null";
        zipText(zip, "summary.json", "{\"retained_intervals\":" + intervals.size() + ",\"p95_interval_ns\":" + (p95 == 0 ? "null" : p95) +
            ",\"p99_interval_ns\":" + (p99 == 0 ? "null" : p99) + ",\"accepted_presents_per_second\":" + fps +
            ",\"estimated_1pct_low_accepted_present_fps\":" + onePercentLow + ",\"slow_intervals\":" + spikes + ",\"paused_intervals\":" + pausedIntervals +
            ",\"frame_samples_included\":" + samplesIncluded + ",\"frame_samples_skipped_older_run\":" + samplesSkipped +
            ",\"scope\":\"retained accepted-present intervals; not display latency or whole-frame GPU time\"}\n");
        zipText(zip, "events.json", "{\"slow_intervals_in_retained_trace\":" + spikes +
            ",\"saved_windows\":" + files(dir, "event").length +
            ",\"slow_interval_timestamps_ns\":[" + spikeTimes + "]" +
            ",\"context\":\"up to 30 seconds before and 10 seconds after; truncated if recording stopped or at the size cap\"}\n");
        // The existing export already warns that runtime logs can contain paths.
        // Keep at most the tail of the newest runtime log rather than all history,
        // and (SR4 S-2) the end of the one before it: after a crash and a restart
        // that is the run that crashed.
        File newest = null, previous = null;
        for (File log : logs) {
            if (!log.getName().startsWith("runtime-")) continue;
            if (newest == null || log.getName().compareTo(newest.getName()) > 0) { previous = newest; newest = log; }
            else if (previous == null || log.getName().compareTo(previous.getName()) > 0) previous = log;
        }
        if (newest != null) zipText(zip, "runtime.log", logTail(newest, 2L * 1024 * 1024));
        if (previous != null) zipText(zip, "previous-runtime.log", logTail(previous, 256L * 1024));
        // What the previous process printed to stderr (aborts, sanitizer and
        // libc messages) and the newest Java crash, both kept by PlayerLogs.
        File javaCrash = null;
        for (File log : logs) {
            if (log.getName().equals("native-stderr.previous.log")) zipText(zip, "previous-native-stderr.log", logTail(log, 64L * 1024));
            if (log.getName().startsWith("java-crash-") && (javaCrash == null || log.lastModified() > javaCrash.lastModified())) javaCrash = log;
        }
        if (javaCrash != null) zipText(zip, "java-crash.txt", logTail(javaCrash, 64L * 1024));
        if (extras != null) {
            try { extras.write(zip); }
            catch (IOException | RuntimeException error) { zipText(zip, "exit-reasons.json", "{\"available\":false,\"reason\":\"export failed\"}\n"); }
        }
        zip.finish(); zip.flush();
    }
    private static long percentile(List<Long> values, int percent) {
        if (values.isEmpty()) return 0; // 0 means unavailable, never a measured zero interval.
        int index = (int) Math.ceil(values.size() * (percent / 100.0)) - 1;
        return values.get(Math.max(0, Math.min(index, values.size() - 1)));
    }
}
