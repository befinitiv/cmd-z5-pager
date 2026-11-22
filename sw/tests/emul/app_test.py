"""The MeshCore app link of the emulator (tests/emul/run_app.sh): drives it with the meshcore library as the app does."""
import asyncio, sys, time
from meshcore import MeshCore, EventType

async def main(port, peer_name):
    mc = await MeshCore.create_tcp("127.0.0.1", port, debug=False)
    ok = 0; fails = []
    def check(name, cond, info=""):
        nonlocal ok
        if cond: ok += 1; print("ok  ", name, info)
        else: fails.append(name); print("FAIL", name, info)
    print("self:", mc.self_info)
    check("self_info", mc.self_info and mc.self_info.get("name", "").startswith("CMD-Z5"), mc.self_info.get("name"))
    r = await mc.commands.send_device_query()
    check("device_query", r.type == EventType.DEVICE_INFO, r.payload)
    r = await mc.commands.get_bat()
    check("battery", r.type == EventType.BATTERY, r.payload)
    r = await mc.commands.get_time()
    check("time", r.type == EventType.CURRENT_TIME, r.payload)
    r = await mc.commands.set_time(int(time.time()))
    check("set_time", r.type == EventType.OK, r.payload)
    for i in range(4):
        r = await mc.commands.get_channel(i)
        check(f"channel {i}", r.type == EventType.CHANNEL_INFO, r.payload)
    r = await mc.commands.set_channel(2, "#cmdz5test", bytes.fromhex(__import__("hashlib").sha256(b"#cmdz5test").hexdigest()[:32]))
    check("set_channel hashtag", r.type == EventType.OK, r.payload)
    r = await mc.commands.get_channel(2)
    check("get_channel 2", r.type == EventType.CHANNEL_INFO and r.payload.get("channel_name") == "#cmdz5test", r.payload)
    r = await mc.commands.get_default_flood_scope()
    check("default scope", r.type != EventType.ERROR, r.payload)
    r = await mc.commands.get_stats_core()
    check("stats core", r.type != EventType.ERROR, r.payload)
    r = await mc.commands.get_stats_packets()
    check("stats packets", r.type != EventType.ERROR, r.payload)
    r = await mc.commands.export_contact()
    check("export self", r.type == EventType.CONTACT_URI, str(r.payload)[:60])
    r = await mc.commands.send_advert(flood=True)
    check("advert", r.type == EventType.OK, r.payload)
    # Wait for the peer's advert.
    peer = None
    for _ in range(30):
        r = await mc.commands.get_contacts(timeout=10)
        if r.type == EventType.CONTACTS:
            for k, c in r.payload.items():
                if c["adv_name"].startswith(peer_name):
                    peer = c
        if peer: break
        await asyncio.sleep(1)
    check("peer contact", peer is not None, peer and (peer["adv_name"], peer["out_path_len"]))
    if peer:
        r = await mc.commands.send_msg(peer, "Hallo von der App")
        check("send_msg", r.type == EventType.MSG_SENT, r.payload)
        if r.type == EventType.MSG_SENT:
            ack = r.payload["expected_ack"].hex()
            e = await mc.wait_for_event(EventType.ACK, attribute_filters={"code": ack}, timeout=20)
            check("ack", e is not None, e and e.payload)
    r = await mc.commands.send_chan_msg(0, "Kanal von der App")
    check("send_chan_msg", r.type == EventType.OK, r.payload)
    # Messages from the peer (it sends one after our advert).
    got = []
    for _ in range(40):
        r = await mc.commands.get_msg(timeout=5)
        if r.type in (EventType.CONTACT_MSG_RECV, EventType.CHANNEL_MSG_RECV):
            got.append(r.payload); print("msg:", r.payload)
        else:
            if any(m.get("text", "").startswith("Gut") for m in got): break
            await asyncio.sleep(1)
    check("received dm", any(m.get("text", "").startswith("Gut") for m in got), [m.get("text") for m in got])
    r = await mc.commands.get_msg(timeout=5)
    check("no more", r.type == EventType.NO_MORE_MSGS, r.type)
    await mc.disconnect()
    print(f"RESULT {ok} ok, {len(fails)} failed: {fails}")
    return 0 if not fails else 1

sys.exit(asyncio.run(main(int(sys.argv[1]), sys.argv[2])))
