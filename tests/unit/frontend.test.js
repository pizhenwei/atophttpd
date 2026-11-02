// Run with: node tests/unit/frontend.test.js
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const context = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, "../../http/js/atop_parse.js"), "utf8"), context);

for (const field of ["gpupercnow", "mempercnow", "gpuperccum", "memperccum", "gpubusy", "gpumembusy"]) {
    for (const value of [-1, "-", "", null, undefined, NaN]) {
        assert.equal(context.Value2ValuePercent(value, field), "-");
    }
    assert.equal(context.Value2ValuePercent(0, field), "0%");
    assert.equal(context.Value2ValuePercent(30, field), "30%");
}
assert.equal(context.Value2ValuePercent(-1, "other"), "-1%");
console.log("Frontend percentage tests passed");
