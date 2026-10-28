# A MeshCore bot for the badge: answers "ping" in a direct message, and
# "!ping" on a channel, with "pong" and how the message came in.
#
#   python ping_bot.py        (LoRa on: Settings > Connection > LoRa)
#   python ping_bot.py SOCKET (another meshcored: the tests' simulated node)
#
# It talks to meshcored, the badge's MeshCore node, which serves one app at
# a time: the MeshCore app takes the node over when it opens, and the bot
# stops (or, with auto_reconnect=True, takes it back).
import asyncio
import sys

from meshcore import EventType, MeshCore


async def main():
    mc = await MeshCore.create_unix(*sys.argv[1:2])
    if mc is None:
        raise SystemExit("meshcored does not answer: is LoRa on?")
    await mc.ensure_contacts()
    print("bot ready as", mc.self_info["name"])

    # One command at a time: meshcore_py tells replies apart by their type
    # only, so the bot handles messages one by one from a queue.
    inbox = asyncio.Queue()
    mc.subscribe(EventType.CONTACT_MSG_RECV, inbox.put_nowait)
    mc.subscribe(EventType.CHANNEL_MSG_RECV, inbox.put_nowait)
    mc.subscribe(EventType.DISCONNECTED, inbox.put_nowait)
    await mc.start_auto_message_fetching()

    while True:
        ev = await inbox.get()
        if ev.type == EventType.DISCONNECTED:
            print("meshcored closed the connection")
            break
        m = ev.payload
        hops = m.get("path_len", 255)  # 255: came by a known (direct) route
        how = "direct route" if hops == 255 else "%d hops" % hops
        if "SNR" in m:
            how += ", SNR %.1f dB" % m["SNR"]
        if ev.type == EventType.CONTACT_MSG_RECV:
            print("DM", m["pubkey_prefix"], repr(m["text"]))
            if m["text"].strip().lower() == "ping":
                await mc.ensure_contacts(follow=True)
                who = mc.get_contact_by_key_prefix(m["pubkey_prefix"])
                if who:
                    await mc.commands.send_msg(who, "pong (%s)" % how)
        else:
            # a channel message's text is "name: text"
            name, _, text = m["text"].partition(": ")
            print("channel", m["channel_idx"], name, repr(text))
            if text.strip().lower() == "!ping":
                # wait while repeaters pass the message on (and the sender's
                # radio sends its acks): a radio that sends hears nothing
                await asyncio.sleep(2)
                await mc.commands.send_chan_msg(m["channel_idx"], "@%s pong (%s)" % (name, how))

    await mc.disconnect()


asyncio.run(main())
