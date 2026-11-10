package io.github.freefrank.lostodyssey;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

// Steprant 2.0, Super Roadmap 4.0 S-2: why the previous runs of the app ended.
// Android 11+ keeps the last process exits of a package (crash, native crash,
// ANR, low memory kill, user stop) with a timestamp, memory figures and, for
// native crashes and ANRs, the system's own trace (a tombstone in protobuf
// form, or the ANR text). The Tester ZIP carries the list as exit-reasons.json
// and the traces of the newest crashes under exits/, so a crash that happened
// before the export is diagnosable. Nothing is read on older Android.
final class TesterExitReasons {
    static final int MAX_EXITS = 8;
    static final int MAX_TRACES = 3;
    static final int MAX_TRACE_BYTES = 2 * 1024 * 1024;

    // One process exit, independent of the Android types so the JSON is testable.
    static final class Exit {
        long timestampMs;
        int pid;
        String reason = "unknown";
        int status;
        int importance;
        long pssKb, rssKb;
        String description = "";
    }

    private TesterExitReasons() { }

    static String reasonName(int reason) {
        switch (reason) {
            case 1: return "exit_self";
            case 2: return "signaled";
            case 3: return "low_memory";
            case 4: return "crash";
            case 5: return "crash_native";
            case 6: return "anr";
            case 7: return "initialization_failure";
            case 8: return "permission_change";
            case 9: return "excessive_resource_usage";
            case 10: return "user_requested";
            case 11: return "user_stopped";
            case 12: return "dependency_died";
            case 13: return "other";
            case 14: return "freezer";
            case 15: return "package_state_change";
            case 16: return "package_updated";
            default: return "unknown";
        }
    }

    static boolean crash(String reason) {
        return reason.equals("crash") || reason.equals("crash_native") || reason.equals("anr");
    }

    private static String quote(String text) {
        StringBuilder out = new StringBuilder("\"");
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c < 0x20) out.append(String.format(Locale.US, "\\u%04x", (int) c));
            else out.append(c);
        }
        return out.append('"').toString();
    }

    static String json(List<Exit> exits, String unavailable) {
        StringBuilder out = new StringBuilder("{");
        if (unavailable != null) {
            out.append("\"available\":false,\"reason\":").append(quote(unavailable)).append("}\n");
            return out.toString();
        }
        out.append("\"available\":true,\"exits\":[");
        for (int i = 0; i < exits.size(); ++i) {
            Exit e = exits.get(i);
            if (i != 0) out.append(',');
            out.append("{\"timestamp_ms\":").append(e.timestampMs)
                .append(",\"pid\":").append(e.pid)
                .append(",\"reason\":").append(quote(e.reason))
                .append(",\"status\":").append(e.status)
                .append(",\"importance\":").append(e.importance)
                .append(",\"pss_kb\":").append(e.pssKb)
                .append(",\"rss_kb\":").append(e.rssKb)
                .append(",\"description\":").append(quote(TesterTraceFiles.redactPaths(e.description)))
                .append('}');
        }
        return out.append("],\"scope\":\"newest first; traces of the newest crashes under exits/\"}\n").toString();
    }

    static String traceName(Exit e) {
        return "exits/exit-" + e.timestampMs + "-" + e.reason + (e.reason.equals("crash_native") ? ".tombstone.pb" : ".txt");
    }

    // Writes exit-reasons.json and the traces into the Tester ZIP.
    static void write(Context context, ZipOutputStream zip) throws IOException {
        if (Build.VERSION.SDK_INT < 30) {
            putText(zip, "exit-reasons.json", json(null, "Android 11 or later is needed"));
            return;
        }
        List<ApplicationExitInfo> infos;
        try {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            infos = manager == null ? null : manager.getHistoricalProcessExitReasons(null, 0, MAX_EXITS);
        } catch (RuntimeException error) {
            putText(zip, "exit-reasons.json", json(null, "unavailable: " + error.getClass().getSimpleName()));
            return;
        }
        List<Exit> exits = new ArrayList<>();
        List<ApplicationExitInfo> withTrace = new ArrayList<>();
        if (infos != null) {
            for (ApplicationExitInfo info : infos) {
                Exit e = new Exit();
                e.timestampMs = info.getTimestamp();
                e.pid = info.getPid();
                e.reason = reasonName(info.getReason());
                e.status = info.getStatus();
                e.importance = info.getImportance();
                e.pssKb = info.getPss();
                e.rssKb = info.getRss();
                CharSequence description = info.getDescription();
                e.description = description == null ? "" : description.toString();
                exits.add(e);
                if (crash(e.reason) && withTrace.size() < MAX_TRACES) withTrace.add(info);
            }
        }
        putText(zip, "exit-reasons.json", json(exits, null));
        for (ApplicationExitInfo info : withTrace) {
            Exit e = exits.get(infos.indexOf(info));
            try (InputStream in = info.getTraceInputStream()) {
                if (in == null) continue;
                zip.putNextEntry(new ZipEntry(traceName(e)));
                byte[] buffer = new byte[16384];
                int total = 0;
                for (int n; total < MAX_TRACE_BYTES && (n = in.read(buffer, 0, Math.min(buffer.length, MAX_TRACE_BYTES - total))) > 0;) {
                    zip.write(buffer, 0, n);
                    total += n;
                }
                zip.closeEntry();
            } catch (IOException | RuntimeException ignored) {
                // A trace the system no longer has is simply left out.
            }
        }
    }

    private static void putText(ZipOutputStream zip, String name, String text) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        zip.closeEntry();
    }
}
