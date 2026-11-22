"""Logins, requests, commands and traces through the emulator's MeshCore app
link, against tests/emul/fake_repeater.py (tests/emul/run_app.sh)."""
import asyncio
import sys

from meshcore import EventType, MeshCore


async def main(port, rep_pub):
    mc = await MeshCore.create_tcp("127.0.0.1", port, debug=False)
    fails = []
    ok = 0

    def check(name, cond, info=""):
        nonlocal ok
        if cond:
            ok += 1
            print("ok  ", name, info)
        else:
            fails.append(name)
            print("FAIL", name, info)

    rep = {"public_key": rep_pub, "type": 2, "flags": 0, "out_path": "", "out_path_len": 0,
           "out_path_hash_mode": 0, "adv_name": "FakeRep", "last_advert": 0,
           "adv_lat": 0.0, "adv_lon": 0.0}
    r = await mc.commands.add_contact(rep)
    check("add repeater (direct route)", r.type == EventType.OK, r.payload)

    e = await mc.commands.send_login_sync(rep, "test", min_timeout=8)
    check("login", e is not None and e.type == EventType.LOGIN_SUCCESS, e and e.payload)

    s = await mc.commands.req_status_sync(rep, min_timeout=8)
    check("status", s is not None and s.get("bat") == 4100, s)

    t = await mc.commands.req_telemetry_sync(rep, min_timeout=8)
    check("telemetry", t is not None, t)

    r = await mc.commands.send_cmd(rep, "ver")
    check("command sent", r.type == EventType.MSG_SENT, r.payload)
    reply = None
    for _ in range(6):
        m = await mc.commands.get_msg(timeout=5)
        if m.type == EventType.CONTACT_MSG_RECV:
            reply = m.payload
            break
        print("  get_msg:", m.type, m.payload)
        await asyncio.sleep(0.5)
    check("command reply", reply is not None and reply.get("txt_type") == 1 and
          reply.get("text") == "> ok: ver", reply)

    r = await mc.commands.send_trace(path=rep_pub[:2], tag=0x1234abcd)
    check("trace sent", r.type == EventType.MSG_SENT, r.payload)
    e = await mc.wait_for_event(EventType.TRACE_DATA, timeout=10)
    check("trace result", e is not None and e.payload.get("tag") == 0x1234abcd, e and e.payload)

    r = await mc.commands.reset_path(rep)
    check("reset path", r.type == EventType.OK, r.payload)
    e = await mc.commands.send_path_discovery_sync(rep, min_timeout=10)
    check("path discovery", e is not None and e.type == EventType.PATH_RESPONSE, e and e.payload)

    # A flooded login: the answer comes in the PATH packet.
    r = await mc.commands.reset_path(rep)
    e = await mc.commands.send_login_sync(rep, "test", min_timeout=8)
    check("login by flood", e is not None and e.type == EventType.LOGIN_SUCCESS, e and e.payload)
    e = await mc.commands.send_login_sync(rep, "wrong", min_timeout=5)
    check("wrong password: no login", e is None or e.type != EventType.LOGIN_SUCCESS,
          e and e.type)

    await mc.disconnect()
    print(f"RESULT {ok} ok, {len(fails)} failed: {fails}")
    return 0 if not fails else 1


sys.exit(asyncio.run(main(int(sys.argv[1]), sys.argv[2])))
