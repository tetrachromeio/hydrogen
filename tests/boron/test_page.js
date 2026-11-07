// Drives Boron's page (boron/native/page) in Chromium: patching keeps elements,
// focus and typing; controls send the right events.
//   node test_page.js <repo-root>      (needs Playwright; exits 0 with a note if missing)
const { execFileSync } = require("child_process");
const path = require("path");

let chromium;
try { ({ chromium } = require("playwright")); } catch (e) {
    console.log("skipped: Playwright is not installed");
    process.exit(0);
}

const root = process.argv[2] || path.join(__dirname, "../..");
const html = execFileSync("python3", [path.join(root, "boron/native/page/gen_page.py"), "--html"]).toString();

let failures = 0;
function check(ok, what) {
    if (!ok) { failures++; console.log("FAIL: " + what); }
}

(async () => {
    const browser = await chromium.launch();
    const page = await browser.newPage();
    // Stand-in for the native side: collect what the page sends.
    const withSink = html.replace("<head>", "<head><script>window.__bnSent = []; " +
        "window.boronSend = (m) => window.__bnSent.push(m.split(String.fromCharCode(31)));</script>");
    await page.setContent(withSink);
    let messages = [];
    const settle = async () => { messages = await page.evaluate(() => window.__bnSent); };
    const last = () => messages[messages.length - 1] || [];
    const patch = (h) => page.evaluate((h) => window.boron.patch(h), h);
    await settle();
    check(messages.length === 1 && last()[0] === "ready", "page sends ready on load");

    const ui = (count, draft, done) =>
        `<h1>Count ${count}</h1><div class="bn-row"><button data-bn="inc" data-bn-click>+1</button>` +
        `<input type="text" data-bn="draft" value="${draft}"></div>` +
        `<label class="bn-check"><input type="checkbox" data-bn="done"${done ? " checked" : ""}>Done</label>`;
    await patch(ui(0, "", false));
    await page.evaluate(() => { window.__btn = document.querySelector("[data-bn=inc]"); });

    await page.click("[data-bn=inc]");
    await settle();
    check(last().join("|") === "click|inc|", "button sends click");

    await patch(ui(1, "", false));
    check(await page.textContent("h1") === "Count 1", "patch updates text");
    check(await page.evaluate(() => window.__btn === document.querySelector("[data-bn=inc]")),
          "patch keeps existing elements");

    await page.click("[data-bn=draft]");
    await page.keyboard.type("milk");
    await settle();
    check(last().join("|") === "input|draft|milk", "typing sends input with the value");
    // The app echoes an older value while the user has typed further: keep their text.
    await patch(ui(1, "mil", false));
    check(await page.inputValue("[data-bn=draft]") === "milk", "a stale echo doesn't overwrite typing");
    check(await page.evaluate(() => document.activeElement.getAttribute("data-bn")) === "draft",
          "focus survives a patch");
    await page.keyboard.press("Enter");
    await settle();
    check(last().join("|") === "submit|draft|milk", "Enter sends submit");
    await patch(ui(1, "", false)); // the app clears the field after adding
    check(await page.inputValue("[data-bn=draft]") === "", "the app can clear a focused field");

    await page.click("[data-bn=done]");
    await settle();
    check(last().join("|") === "change|done|true", "checkbox sends change true");
    await patch(ui(1, "", false)); // the app refuses the change
    check(await page.isChecked("[data-bn=done]") === false, "checkbox follows the app's state");

    await patch("<p>gone</p>");
    check(await page.evaluate(() => document.getElementById("bn-root").innerHTML) === "<p>gone</p>",
          "patch removes old elements");
    await page.evaluate(() => window.boron.css("p { color: rgb(255, 0, 0) }"));
    check(await page.evaluate(() => getComputedStyle(document.querySelector("p")).color) === "rgb(255, 0, 0)",
          "css() applies app styles");

    await browser.close();
    if (failures) process.exit(1);
    console.log("boron page: all checks passed");
})().catch((e) => { console.log("FAIL: " + e.message); process.exit(1); });
