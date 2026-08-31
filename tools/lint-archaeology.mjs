// Fail the build when internal chronology reappears in public-shipping source.
//
// The source under `src/` and `include/` ships to the public verbatim, and the tracker it grew up
// beside does not. A comment reading "see ticket 45" or "finding 7c" or "A45.9" therefore points a
// stranger at nothing, and a trace-log filename points them at a file that exists on one machine.
// The sweep that removed them was a one-off; this is what keeps them out, and it runs identically
// here and in CI so a comment is caught where it is written rather than at release assembly.
//
// WHAT IT DOES NOT DO: judge whether a comment is useful. A mechanism fact is worth keeping and its
// citation is not, so the patterns below match only the citation. Anything this flags is meant to
// be rewritten without its reference, not deleted with it.
//
//   node tools/lint-archaeology.mjs                 # lint src/ and include/
//   node tools/lint-archaeology.mjs path [path...]  # lint the given files or directories
//
// Exits 0 when clean, 1 on any hit, 2 on a usage error.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative, resolve, sep } from "node:path";
import process from "node:process";

const REPO_ROOT = resolve(import.meta.dirname, "..");
const DEFAULT_TARGETS = ["src", "include"];
const SOURCE_EXTENSIONS = [".h", ".hpp", ".cpp", ".c", ".cc", ".inl", ".ixx"];

const RULES = [
  {
    name: "ticket reference",
    pattern: /\bticket\s+\d+/gi,
    hint: "keep the fact, drop the ticket number",
  },
  {
    name: "finding reference",
    pattern: /\bfinding\s+\d+/gi,
    hint: "keep the measurement, drop the finding number",
  },
  {
    name: "acceptance-cell id",
    pattern: /\bA\d+\.\d+\b/g,
    hint: "an acceptance cell only exists in the private tracker",
  },
  {
    name: "tracker path",
    pattern: /\.scratch\//g,
    hint: "the tracker does not ship",
  },
  {
    name: "date cited as evidence",
    pattern: /\b20\d{2}-\d{2}-\d{2}\b/g,
    hint: "git history carries when; the comment should carry what",
  },
  {
    name: "trace-log filename",
    pattern: /\bT\d+-[\w.-]*\.log\b/gi,
    hint: "a trace log exists on one machine",
  },
];

function collect(target) {
  const absolute = resolve(REPO_ROOT, target);
  let info;
  try {
    info = statSync(absolute);
  } catch {
    console.error(`lint-archaeology: no such path: ${target}`);
    process.exit(2);
  }
  if (info.isFile()) return [absolute];
  const found = [];
  for (const entry of readdirSync(absolute, { withFileTypes: true })) {
    const child = join(absolute, entry.name);
    if (entry.isDirectory()) {
      found.push(...collect(child));
    } else if (SOURCE_EXTENSIONS.some((ext) => entry.name.toLowerCase().endsWith(ext))) {
      found.push(child);
    }
  }
  return found;
}

const targets = process.argv.slice(2);
if (targets.includes("--help") || targets.includes("-h")) {
  console.log("usage: node tools/lint-archaeology.mjs [path...]   (default: src include)");
  process.exit(0);
}

const files = (targets.length > 0 ? targets : DEFAULT_TARGETS).flatMap(collect);
const hits = [];

for (const file of files) {
  const lines = readFileSync(file, "utf8").split(/\r?\n/);
  lines.forEach((line, index) => {
    for (const rule of RULES) {
      rule.pattern.lastIndex = 0;
      for (const match of line.matchAll(rule.pattern)) {
        hits.push({
          file: relative(REPO_ROOT, file).split(sep).join("/"),
          line: index + 1,
          rule: rule.name,
          hint: rule.hint,
          text: match[0],
        });
      }
    }
  });
}

if (hits.length === 0) {
  console.log(`lint-archaeology: clean across ${files.length} file(s)`);
  process.exit(0);
}

for (const hit of hits) {
  console.log(`${hit.file}:${hit.line}: ${hit.rule} "${hit.text}" -- ${hit.hint}`);
}
const byRule = new Map();
for (const hit of hits) byRule.set(hit.rule, (byRule.get(hit.rule) ?? 0) + 1);
const summary = [...byRule].map(([rule, count]) => `${count} ${rule}`).join(", ");
console.log(`\nlint-archaeology: ${hits.length} hit(s) across ${files.length} file(s) -- ${summary}`);
process.exit(1);
