// The install's / rebuild's progress, read from setup_mac.sh's output: each
// step starts with a "==> <what>" line (mac_common.sh's step(); zsh's colour
// escapes stripped), the next one ends it, "==> Done" ends the run. A step
// setup_mac.sh finds done already prints nothing and shows as skipped.
// Homebrew's own "==> Downloading …" lines match no step and are ignored. The
// weights are a step's rough share of a first install (the lift and the
// compile dominate).

export const STEP_IDS = ["brew", "venv", "disc", "decrypt", "lift", "promote", "build"] as const;
export type StepId = (typeof STEP_IDS)[number];
const WEIGHT: Record<StepId, number> = { brew: 12, venv: 4, disc: 1, decrypt: 2, lift: 30, promote: 8, build: 43 };

/** setup_mac.sh's step lines, by the text after "==> ". */
const PATTERNS: [RegExp, StepId][] = [
  [/^Homebrew packages/, "brew"],
  [/^Python environment/, "venv"],
  [/^Game files:/, "disc"],
  [/^Building the EBOOT decrypter|^Decrypting EBOOT/, "decrypt"],
  [/^Lifting the game code/, "lift"],
  [/^Promoting guest registers/, "promote"],
  [/^Building \(folklore\/build-mac\)/, "build"],
];

export interface Step {
  id: StepId;
  state: "pending" | "running" | "done" | "error" | "skipped";
  start: number;
  secs: number;
}

const ESC = /\x1b\[[0-9;]*[A-Za-z]/g;

export class RebuildProgress {
  steps: Step[] = [];
  log: string[] = [];
  cancelled = false;
  started = 0;
  /** "rebuild" (Advanced), "install" (Home's Install the game), or "pull"
   * for Updates > Get the new sources (no steps). */
  kind: "rebuild" | "install" | "pull" = "rebuild";
  /** The "error: ..." lines setup_mac.sh printed (mac_common.sh's die). */
  errors: string[] = [];

  reset(kind: "rebuild" | "install" | "pull" = "rebuild") {
    this.kind = kind;
    this.log = [];
    this.errors = [];
    this.cancelled = false;
    this.started = Date.now();
    this.steps = kind === "pull" ? [] : STEP_IDS.map((id) => ({ id, state: "pending", start: 0, secs: 0 }));
  }

  /** The step that is running, or the one that failed. */
  stage(): Step | undefined {
    return this.steps.find((s) => s.state === "running" || s.state === "error");
  }

  private current(): Step | undefined {
    return this.steps.find((s) => s.state === "running");
  }

  private close(state: Step["state"]) {
    const c = this.current();
    if (c) {
      c.state = state;
      c.secs = (Date.now() - c.start) / 1000;
    }
  }

  feed(raw: string) {
    const line = raw.replace(ESC, "");
    this.log.push(line);
    if (this.log.length > 20000) this.log.splice(0, this.log.length - 20000);
    const err = line.match(/^error:\s*(.*)$/);
    if (err) this.errors.push(err[1]);
    const m = line.match(/^==>\s+(.*)$/);
    if (!m) return;
    const text = m[1];
    if (/^Done\b/.test(text)) {
      this.close("done");
      return;
    }
    const hit = PATTERNS.find(([re]) => re.test(text));
    if (!hit) return;
    const step = this.steps.find((s) => s.id === hit[1]);
    if (!step || step.state === "running") return;
    this.close("done");
    // steps the script went past without a line of their own (done before)
    for (const s of this.steps) {
      if (s === step) break;
      if (s.state === "pending") s.state = "skipped";
    }
    step.state = "running";
    step.start = Date.now();
  }

  finish(ok: boolean) {
    this.close(ok ? "done" : "error");
    if (ok) for (const s of this.steps) if (s.state === "pending") s.state = "skipped";
  }

  fraction(): number {
    const total = this.steps.reduce((a, s) => a + WEIGHT[s.id], 0);
    if (!total) return 0;
    let done = 0;
    for (const s of this.steps) {
      if (s.state === "done" || s.state === "skipped") done += WEIGHT[s.id];
      else if (s.state === "running") {
        // a running step counts as half done after its usual share of time
        const el = (Date.now() - s.start) / 1000;
        done += WEIGHT[s.id] * Math.min(0.9, el / (el + 120));
      }
    }
    return done / total;
  }

  /** Minutes left, from the time so far and the share done; null early on. */
  etaMinutes(): number | null {
    const f = this.fraction();
    const el = (Date.now() - this.started) / 60000;
    if (f < 0.05 || el < 0.5) return null;
    return (el / f) * (1 - f);
  }
}
