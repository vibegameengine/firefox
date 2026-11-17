import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";

const require = createRequire("C:/GameEngien/hyperreal-starter-kit/");
const { chromium } = require("playwright-core");

const label = process.argv[2] ?? "chromeprofile";
const seconds = Number(process.argv[3] ?? 6);
const results = "C:/Projects/gecko-build-logs/bench";
const tools = "C:/Projects/gecko/vge/bedrock/tools";
const chrome = process.env.CHROME_EXE ?? "C:/Users/pavel/AppData/Local/ms-playwright/chromium-1228/chrome-win64/chrome.exe";
const slots = path.join(os.homedir(), ".claude/skills/browser-slots/browser-slots.mjs");
const name = `chromeprofile-${label}`;

execFileSync("node", [slots, "acquire-perf", name, "--yes-timing", "Chromium CPU profile of the game start"], { stdio: "ignore" });
let browser;
try {
  browser = await chromium.launch({ headless: false, executablePath: chrome, args: ["--window-size=1280,800"] });
  const page = await browser.newPage({ viewport: { width: 1264, height: 761 } });
  const lines = [];
  page.on("console", (message) => {
    if (message.text().includes("frameprobe")) lines.push(message.text());
  });
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Profiler.enable");
  await cdp.send("Profiler.setSamplingInterval", { interval: 1000 });
  await cdp.send("Profiler.start");
  await page.goto(process.env.BENCH_URL ?? "https://claybound-56949.web.app/");
  await page.evaluate(readFileSync(`${tools}/../test/frame-probe.js`, "utf8"));
  await new Promise((resolve) => setTimeout(resolve, seconds * 1000));
  const { profile } = await cdp.send("Profiler.stop");
  writeFileSync(`${results}/${label}.cpuprofile`, JSON.stringify(profile));
  writeFileSync(`${results}/${label}.log`, lines.join("\n"));
  console.log(JSON.stringify({ label, nodes: profile.nodes.length, samples: profile.samples?.length ?? 0 }));
} finally {
  await browser?.close();
  execFileSync("node", [slots, "release", name], { stdio: "ignore" });
}
