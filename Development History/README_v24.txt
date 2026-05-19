myOS v24 - Coalesced SharedBus / Event-Lane

Neu:
- SharedBus Producer/Consumer nutzt jetzt dirty/notify coalescing.
- Producer schreibt Payload in shared section.
- Nur die erste noch ausstehende BUSLAB_NOTIFY wird gepostet.
- Weitere Writes coalescen in denselben pending dirty signal.
- Producer: Spam 10k Button.

Test:
1. Start -> SharedBusLab
2. Producer: Create Bus
3. Consumer: Map Bus
4. Producer: Spam 10k
5. Consumer sollte latest version lesen, statt 10000 Logs abzuarbeiten.

Erwartung:
- writes steigt stark
- posted signals bleibt klein
- coalesced signals steigt stark
- Consumer bleibt responsive und liest latest payload.
