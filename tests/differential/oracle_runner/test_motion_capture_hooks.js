// Exercise the real Frida hook callbacks without an Android process. Only the
// native memory reads and Interceptor registration are substituted here.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

function recorder() {
    const context = vm.createContext({
        rpc: { exports: {} },
        __FRAME_SELECTION_PROJECTION_JSON__: {},
    });
    vm.runInContext(fs.readFileSync(
        path.join(__dirname, 'frida_motion_stage_agent.js'), 'utf8'), context);
    return vm.runInContext(`(() => {
        const hooks = {};
        attachAt = (offset, name, callbacks) => { hooks[name] = callbacks; };
        readVariantArg = (args) => args;
        ptrHex = value => value === null ? null : String(value);
        walkNodes = (player) => ({
            layout: 'deque', layers: [{posX: player.value}],
        });
        recording = true;
        enabledStages = new Set([...ALL_STAGES, ...RENDER_STAGES]);
        installTraceFlattenHooks();
        return {
            enter(name, delta, integer = false) {
                const ctx = {};
                const obj = { toString: () => name };
                hooks.Player_progressCompat.onEnter.call(ctx, [
                    null, {toInt32: () => 1},
                    {scalar: {type: integer ? 4 : 5, double: delta},
                     variant: {readS64: () => ({toNumber: () => delta})}}, obj,
                ]);
                return ctx;
            },
            sample(name, value) {
                const player = {value, toString: () => name};
                const ctx = {};
                hooks.Player_phase3_last.onEnter.call(ctx, [player]);
                hooks.Player_phase3_last.onLeave.call(ctx);
                player.value = -999; // cleanup/callback mutates live memory
            },
            leave(ctx) { hooks.Player_progressCompat.onLeave.call(ctx); },
            result() { return JSON.parse(JSON.stringify(events)); },
            active() { return {inCompat, currentFrameId}; },
            completed() { return rpc.exports.eventCount(); },
            render(accurateSla) {
                currentRenderFrameId = 0;
                emitRender(STAGE_RENDER_EXECUTE, 'execute_enter', {accurateSla}, {});
                emitRender(STAGE_RENDER_EXECUTE, 'execute_leave', {accurateSla}, {});
            },
        };
    })()`, context);
}

{
    const r = recorder();
    const frame = r.enter('root', 0);
    assert.equal(r.completed(), 0); // host polling must not see an unfinished frame
    r.sample('grandchild', 3);
    r.sample('child', 2);
    r.sample('root', 1);
    r.leave(frame);
    assert.equal(r.completed(), 1);
    const event = r.result()[0];
    assert.deepEqual(Array.from(event.layers, x => x.posX), [3, 2, 1]);
    assert.equal(event.diagnostics.topPlayer, 'root');
    assert.equal(event.samplePoint, 'progressCompat.phase3-end.pre-cleanup');
    assert.equal(event.sampleOrder, 'phase3-return-order');
    assert.deepEqual(Array.from(event.playerLayerCounts), [1, 1, 1]);
    assert.equal(event.deltaMs, 0);
}

{
    const r = recorder();
    const outer = r.enter('outer', 67, true);
    r.sample('outer-child', 1);
    const inner = r.enter('inner', 66);
    assert.equal(r.completed(), 0);
    r.sample('inner', 2);
    r.leave(inner);
    assert.equal(r.completed(), 1);
    assert.equal(r.active().currentFrameId, 0);
    r.sample('outer', 3);
    r.leave(outer);
    assert.equal(r.completed(), 2);
    const events = r.result();
    assert.deepEqual(Array.from(events, e => e.frameId), [1, 0]);
    assert.deepEqual(Array.from(events[1].layers, x => x.posX), [1, 3]);
    assert.deepEqual(Array.from(events, e => e.deltaMs), [66, 67]);
    assert.equal(r.active().inCompat, false);
}
{
    for (const accurate of [false, true]) {
        const r = recorder();
        r.render(accurate);
        const boundary = accurate ? 'Player.renderAccurateSeparateLayerAdaptor'
                                  : 'Player.renderToCanvas';
        const events = r.result();
        assert.deepEqual(Array.from(events, e => e.executeBoundary), [boundary, boundary]);
        assert.deepEqual(Array.from(events, e => e.samplePoint),
                         [boundary + '.enter', boundary + '.leave']);
        assert.equal(events[0].captureContract, 'motion-render-boundaries-v1');
    }
}
process.stdout.write('motion capture hooks: snapshots, nested windows and render boundaries PASS\n');
