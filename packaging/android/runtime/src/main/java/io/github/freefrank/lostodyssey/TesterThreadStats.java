package io.github.freefrank.lostodyssey;

/** Linux /proc/self/task/<tid>/stat parser; no device-global process access. */
final class TesterThreadStats {
    final String name, state;
    final long ticks, startTicks;
    private TesterThreadStats(String name, String state, long ticks, long startTicks) {
        this.name = name; this.state = state; this.ticks = ticks; this.startTicks = startTicks;
    }
    static TesterThreadStats parse(String line) {
        if (line == null) return null;
        int left = line.indexOf('('), right = line.lastIndexOf(')');
        if (left < 0 || right <= left || right + 2 >= line.length()) return null;
        String[] fields = line.substring(right + 2).trim().split("\\s+");
        if (fields.length <= 19) return null;
        try {
            String name = line.substring(left + 1, right).replace(',', '_').replace('\n', '_');
            return new TesterThreadStats(name, fields[0], Long.parseLong(fields[11]) + Long.parseLong(fields[12]),
                Long.parseLong(fields[19]));
        } catch (NumberFormatException invalid) { return null; }
    }
    /**
     * Steprant 2.1.1: /proc/self/task/<tid>/schedstat, "run_ns wait_ns timeslices":
     * time on a CPU, time runnable but waiting for one, and times scheduled in.
     */
    static long[] parseSchedstat(String line) {
        if (line == null) return null;
        String[] fields = line.trim().split("\\s+");
        if (fields.length < 3) return null;
        try {
            long[] values = {Long.parseLong(fields[0]), Long.parseLong(fields[1]), Long.parseLong(fields[2])};
            return values[0] >= 0 && values[1] >= 0 && values[2] >= 0 ? values : null;
        } catch (NumberFormatException invalid) { return null; }
    }
}
