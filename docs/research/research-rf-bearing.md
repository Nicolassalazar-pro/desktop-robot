# Rotational RF Bearing and ESP32-S3 Radio Capability for Yaw-Drift Correction

## Verdict

- **[DERIVED] Spin-and-measure WiFi bearing is dead on arrival, and the phase-centre suspicion in the brief is exactly right.** Rotating an antenna about its own phase centre produces zero change in range-to-AP, hence zero propagation-phase modulation, hence zero synthetic aperture. Only the element pattern rotates.
- **[DERIVED] Even the best-case off-axis geometry does not rescue it.** With the antenna at most r = 3.5 cm from the spin axis of a 7 cm robot, aperture L = 2r = 7 cm = 0.57 λ at 2.44 GHz (λ = 12.29 cm). Uniform-aperture beamwidth ≈ 0.886 λ/L = **89°**. Unusable, and unreachable anyway (§Findings 1.2).
- **[SOURCED] The closest published test of this exact idea is a negative result.** ESP32-ROOM-DA + BNO085 IMU, **two** antennas, 7 APs, 100 fingerprint training points, DJI RoboMaster S1: best classifier k-NN achieved **MCC 0.23, F1 0.39** on 4-class (90°-granularity) direction (DOI 10.2478/ama-2025-0079).
- **[DERIVED] FTM/RTT does not supersede it — it solves a different problem.** ESP32-S3 supports 802.11mc FTM (`SOC_WIFI_FTM_SUPPORT (1)`), but RTT yields **range**, and range is invariant under spin-in-place. Zero yaw information. Android quotes 1–2 m with ≥3 APs; that is ~30× this robot's body length.
- **[DERIVED] CSI cannot do AoA on this part.** ESP32-S3 has one RF chain; `esp_phy_set_ant()` is an external *switch*, not a second receiver. No cross-antenna ratio ⇒ no way to cancel per-packet CFO/SFO/PDD phase ⇒ no phase-based DoA.
- **[DERIVED] Against tonight's measured baseline the entire RF programme is pointless.** Heading holds ±0.05° over 48 s with ZUPT; coast overshoot is 7–9° and is ~300× the residual drift. Best-case RF bearing error is 25–90°. **Fixing coast overshoot is worth ~1000× more than any RF work described here.**

## Findings

### 1. Rotational bearing theory

#### 1.1 The phase-centre caveat is fatal

**[DERIVED]** A synthetic aperture requires *translating* the phase centre so path length varies knowably: Δd(θ) = x·cos(θ − θ_AP), Δφ = (2π/λ)·Δd. If the phase centre sits on the rotation axis, far-field range is rotation-invariant, Δd ≡ 0, Δφ ≡ 0. What remains is only the complex element response G(θ_DoA − ψ) for robot heading ψ.

**[DERIVED]** With offset r, at 2.44 GHz (λ = c/f = 299 792 458 / 2.44e9 = 0.1229 m):

| quantity | r = 2.0 cm | r = 3.5 cm |
|---|---|---|
| Aperture L = 2r | 4.0 cm = 0.33 λ | 7.0 cm = 0.57 λ |
| Peak-to-peak phase, 2π·2r/λ | 117° | 205° |
| 3-dB beamwidth ≈ 0.886 λ/L | 156° | **89°** |

**[DERIVED]** Two further blockers make even 89° unreachable: (a) commodity WiFi CSI carries random per-packet carrier-frequency offset, sampling-frequency offset and packet-detection-delay terms, and one RF chain gives no reference to divide them out; (b) you would need r known to ~λ/20 ≈ 6 mm, and the antenna phase-centre location is unpublished for this board.

#### 1.2 The on-board antenna is not directional

**[SOURCED]** Waveshare specifies "Onboard patch antenna" (item ⑩), single antenna, no IPEX, no RF switch. **[SOURCED]** Espressif's only public directivity statement: "The effect of external IPEX antenna is better than PCB antenna, **PCB antenna has directivity**" (`espressif/esp-csi:README.md` §5 Note 1) — qualitative, no dB. **[SOURCED]** Espressif treats the pattern as geometry-dependent and unspecified: "For PCB antennas, make sure to validate them through both simulation and real-world testing."

**[INFERRED]** An electrically small IFA/chip antenna on a ~4 cm ground plane: peak gain 0–2 dBi, azimuth ripple **3–10 dB peak-to-peak**, front-to-back **0–6 dB**. Contrast a real RDF Yagi/patch: 15–25 dB F/B, 30–60° main lobe. The LiPo pouch and LCD frame are ~0.3–0.4 λ across — they diffract rather than shield, so no deep back null.

#### 1.3 Resolution from lobe width and RSSI noise

**[DERIVED]** Parabolic-peak model P(θ) = P₀ − (12/θ₃²)(θ−θ₀)² dB gives peak-location error σ_θ ≈ (θ₃/2)·√(σ/3 dB)/√N_eff. With θ₃ ≈ 120°, σ ≈ 1.5 dB, N_eff ≈ 12 → **σ_θ ≈ 12°**. **[DERIVED] This number must not be quoted as expected accuracy** — it is the noise-limited bound only. The dominant term is model error: the measured curve is not the antenna pattern, and multipath displacement is a *bias* of tens of degrees that averaging does not reduce.

### 2. Multipath destroys the peak

**[DERIVED]** P(ψ) = |Σ_k a_k·G(θ_k − ψ)·e^{jφ_k}|². Indoors many components have comparable magnitude, so (a) the maximum aligns the pattern with the strongest *cluster* — frequently a wall/metal reflection, not LOS — and (b) since |G| varies only a few dB, interference between components dominates the curve shape. You measure the local standing-wave field, not the pattern.

**[SOURCED]** SpotFi's second contribution exists solely because the strongest peak is not the direct path: "novel filtering and estimation techniques to identify AoA of **direct path** … by assigning values for each path depending on how likely the particular path is the direct path" — and it still needed **3 antennas** + super-resolution for 40 cm median (DOI 10.1145/2785956.2787487).

**[SOURCED]** The single-antenna pattern-based AoA method that *does* work is UWB: Ledergerber & D'Andrea, "does not require multiple transmitter or receiver antennas, or relative motion … solely based on the angle-dependent impulse response function", applied to robot self-localisation (DOI 10.3390/s19204466). **[DERIVED]** It works because ~500 MHz bandwidth resolves the direct path in time first. A 20 MHz WiFi channel has time resolution c/B = 3e8/2e7 = **15 m** — it cannot separate direct from any indoor reflection.

**[DERIVED]** Repeatability: at a fixed spot in a static room the curve is deterministic and should correlate well rev-to-rev. Across locations it will **not** rotate correctly. Small-scale fading decorrelates at ≈0.38 λ ≈ **4.7 cm** (first null of Clarke/Jakes J₀(2πd/λ)); a 10 cm desk move is ~2 correlation lengths. **[DERIVED]** Worse, if r = 2–3.5 cm the antenna traverses 2πr = 12.6–22 cm ≈ 1–1.8 λ per revolution — ~2.5–4.7 independent fading samples, injecting several random lobes that look exactly like an antenna pattern.

**[SOURCED]** Zhou et al.'s Radio Irregularity Model states the trap in one line: "the variance in received signal strength is **largely random**; however, it exhibits a **continuous change with incremental changes in direction**" (DOI 10.1145/1149283.1149287). Smooth in heading, so it looks like a pattern; random in shape, so it encodes no bearing.

### 3. ESP32-S3 radio capability (ESP-IDF v5.3.1)

#### 3.1 Scan timing

**[SOURCED]** `esp_wifi_types_generic.h`:
```c
#define WIFI_ACTIVE_SCAN_MIN_DEFAULT_TIME 0
#define WIFI_ACTIVE_SCAN_MAX_DEFAULT_TIME 120
#define WIFI_PASSIVE_SCAN_DEFAULT_TIME 360
#define WIFI_SCAN_HOME_CHANNEL_DWELL_DEFAULT_TIME 30
```
**[SOURCED]** Background scans add 30 ms back-to-home per channel. `wifi_scan_config_t` supports `.bssid`, `.channel` ("If 'channel' is 0, all-channel scan; otherwise specific-channel"), `.channel_bitmap`. Header warns "values above 1500ms may cause station to disconnect". **[SOURCED]** "the Wi-Fi driver **does not support multiple concurrent scans adequately** … concurrent scans should be avoided." **[SOURCED]** Arduino default `max_ms_per_chan = 300` (`WiFiScan.h:37-40`), `_scanActiveMinTime = 100` (`WiFiScan.cpp:52`).

**[DERIVED]** Against a 1260 ms revolution: Arduino all-channel ≈ 3.9 s → 0.3 samples/rev. IDF default ≈ 1.6 s → 0.8. All-channel at max=30 ms ≈ 0.4 s → ~3 (120°/sample). **Single-channel active, max=30 ms ≈ 35–50 ms → 25–36 samples/rev** — the only scan-based method that fits.

#### 3.2 Sniffing is the right primitive

**[SOURCED]** "The Wi-Fi sniffer mode can be enabled in the Wi-Fi mode of `WIFI_MODE_NULL`, `WIFI_MODE_STA`, `WIFI_MODE_AP`, or `WIFI_MODE_APSTA`" — **works with softAP up, no mode switch, no joystick drop**. Caveat: "the sniffer has a **great impact** on the throughput" (negligible for a joystick WebSocket), and "the callback will be called **directly in the Wi-Fi driver task**".

**[SOURCED]** Per-packet metadata (`esp_wifi_types_native.h`, `wifi_pkt_rx_ctrl_t`): `signed rssi:8` (dBm), `rate:5`, `sig_mode:2`, `mcs:7`, `cwb:1`, `stbc:2`, `signed noise_floor:8` (present on S3), `channel:4`, `timestamp:32` ("unit: microsecond … precise only if modem sleep or light sleep is not enabled"), `ant:1`, `sig_len:12`.

**[SOURCED]** Beacon floor: `wifi_ap_config_t.beacon_interval` — "Unit: TU (time unit, **1 TU = 1024 us**) … Default value: 100" ⇒ **102.4 ms** per BSSID. **[SOURCED]** `esp_wifi_sta_get_rssi()` returns "rssi info received from **last beacon**", STA/APSTA only, associated AP only.

**[DERIVED]** Sample-rate arithmetic for 36 samples/rev (10°): beacon sniffing needs 36 × 102.4 ms = **3.69 s/rev = 97.6 °/s** — a 3× slowdown from the current 285 °/s. The existing 1260 ms spin yields only **12 beacon samples/rev (30°/sample)**. Three-channel hopping at 110 ms dwell → 330 ms/AP → 11.9 s/rev = 30 °/s.

#### 3.3 CSI

**[SOURCED]** `SOC_WIFI_CSI_SUPPORT (1)` in `esp32s3/soc_caps.h`. Contents: "channel frequency responses of sub-carriers … two bytes of signed characters. The first one is **imaginary part** and the second one is **real part**" — amplitude *and* phase. Non-HT 20 MHz = 128 B (sub-carriers 0..31, −32..−1); HT20 = 256 B; HT40 up to 612 B. 802.11n 20 MHz = 56 total / 52 usable sub-carriers; pilots at −21, −7, +7, +21. `wifi_csi_info_t { rx_ctrl; mac[6]; dmac[6]; first_word_invalid; int8_t *buf; len; hdr; payload; payload_len; rx_seq; }`. Config: `lltf_en`, `htltf_en`, `stbc_htltf2_en`, `ltf_merge_en`, `channel_filter_en`, `manu_scale`/`shift`, `dump_ack_en`. Documented limits: `first_word_invalid` ("first four bytes … invalid due to a hardware limitation"), STBC CSD recorded only for the first space-time stream, CSD[10:0] = 11 bits over ±π. **[SOURCED]** "it is suggested to enable sniffer mode to receive more CSI data" — CSI from arbitrary APs' beacons, no association needed.

**[DERIVED]** CSI's only real value here is as a richer per-heading feature (52 sub-carrier amplitudes vs. one RSSI scalar) for a fingerprinting fallback. Phase-slope ToF is corrupted by SFO/PDD and capped at 15 m resolution by the 20 MHz bandwidth.

#### 3.4 FTM

**[SOURCED]** `SOC_WIFI_FTM_SUPPORT (1)`; ESP32-S3 listed in `examples/wifi/ftm/README.md` supported targets. Roles: "Initiator in station mode … Responder in AP mode". `esp_wifi.h`: "@attention 1. **Use this API only in Station mode.** @attention 2. If FTM is initiated on a different channel than Station is connected in or internal SoftAP is started in, FTM defaults to a single burst in ASAP mode." Config `wifi_ftm_initiator_cfg_t { resp_mac[6]; channel; frm_count (0/16/24/32/64); burst_period (×100 ms); use_get_report_api; }`; calibration via `esp_wifi_ftm_resp_set_offset(int16_t offset_cm)`.

**[SOURCED]** Espressif: "Distance measurement using RTT **is not accurate** … suggested to perform FTM between two ESP32 chip series devices"; "requires **line-of-sight** … no less than **−70 dBm** RSSI"; worked example 26/31 valid readings, avg raw RTT 49.218 ns → 5.07 m. Android: "typically accurate within **1-2 meters**" with ≥3 APs; "The access point must implement the IEEE 802.11-2016 FTM standard". **[SOURCED]** `wifi_ap_record_t` exposes `ftm_responder:1` and `ftm_initiator:1` — one ordinary scan tells you empirically which desk APs respond.

## Applicability to this robot

**Do NOT build the spin-and-measure bearing estimator.** Expected gain: **negative**. Best-case bearing error 25–90° (§1.1, §1.3) against a heading loop already holding **±0.05° over 48 s with ZUPT** (29 zero-updates, bias re-estimated, gyro bias ~0.53 dps). Fusing a 25–90° measurement into a 0.05° estimate can only corrupt it. Cost: ~2–3 days firmware plus a 3× spin slowdown (285 → 98 °/s) to reach 36 samples/rev.

**Do NOT pursue FTM.** Expected gain: **zero for yaw** — RTT is range, and range is invariant under spin-in-place. Even as a position aid it needs ≥3 802.11mc responders (rare in consumer gear) for 1–2 m, versus a 7 cm robot. Cost: mode change to STA per `esp_wifi.h`, which drops the softAP joystick link.

**Do NOT pursue CSI AoA.** Expected gain: **zero** — one RF chain, `esp_phy_set_ant()` is a switch not a second receiver, so per-packet CFO/SFO/PDD phase cannot be cancelled. Cost: high (128–612 B/packet, CSI callback in the Wi-Fi driver task competing with servo timing on a 240 MHz part).

**DO spend the 30-minute pre-check before any of the above is ever reconsidered.** Stop at 8 headings 45° apart; at each run 5× `WiFi.scanNetworks(false,false,false,120)` and average per BSSID. **Gate: if no BSSID shows ≥6 dB peak-to-peak across the 8 headings with 5-scan spread <2 dB, the question is closed.** Cost: ~30 min, no architectural change. **[INFERRED]** I expect this gate to fail.

**DO spend the effort on coast overshoot instead.** Overshoot is **7–9° at full speed** and is ~300× the residual drift budget. The measured servo model `rate = 0.91·speed − 24.2 dps` with deadband at command ~27 gives a directly invertible feed-forward: brake to the deadband edge at a predicted lead angle rather than cutting motors. **[DERIVED]** Removing 8° of overshoot is worth ~1000× more than the best conceivable RF bearing fix, and costs a fraction of the effort.

**DO keep the gravity-projected yaw rate that is already live.** Resting gravity (1.01, 0.10, −0.10) g means single-axis integration understates rotation by **3.48° per 360°** — a 0.97% scale error, ~44× larger than the ZUPT residual and second only to overshoot.

**If a heading fingerprint is ever wanted anyway:** the only defensible version is *place-conditioned* — store a home-position spin signature (CSI amplitude, not RSSI) and reset yaw only when confident the robot is at home. **[DERIVED]** It is not bearing estimation, it needs a calibration spin per location, it dies on a 10 cm move (4.7 cm decorrelation length), and it fails silently when a person sits down.

## Ranked recommendations

| Technique | Expected gain | Implementation cost | Risk | Verdict |
|---|---|---|---|---|
| Coast-overshoot feed-forward from `rate = 0.91·speed − 24.2` | **7–9° → target <1°**; dominant error removed | Low — model already measured | Low; re-tune per battery voltage | **Do this first** |
| Gravity-projected yaw rate (already live) | 3.48°/360° scale error removed (0.97%) | Done | Low | **Keep** |
| 8-heading stop-and-scan pre-check | Information only: closes the RF question | ~30 min, no firmware change | None | **Do, then stop** |
| Promiscuous beacon sniffing on softAP channel (data collection only) | 12 samples/rev now; 36 at 98 °/s | Medium — callback + µs-timestamp interp | Sniffer "great impact on throughput"; channel-locked to softAP | Only if pre-check passes |
| Place-conditioned CSI-amplitude home fingerprint | Yaw reset **at home only**; ±20° at best | High — CSI plumbing + matcher | Dies on 10 cm move; silent failure | Not worth it |
| Spin-and-measure absolute bearing (RSSI) | 25–90° error vs. 0.05° baseline | High + 3× spin slowdown | Actively corrupts the estimate | **No** |
| CSI-based AoA | **Zero** — 1 RF chain | Very high | — | **No** |
| FTM / 802.11mc RTT | **Zero for yaw**; 1–2 m position | Medium; drops softAP | Few responders; needs LOS ≥ −70 dBm | **No** |
| External 2nd antenna + RF switch (hardware mod) | Only credible RF-AoA path on this silicon | Very high — PCB rework | λ/2 = 6.1 cm spacing on a 7 cm robot | Out of scope |

## Could not verify

- **Measured azimuth pattern / front-to-back ratio for this antenna.** `esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf` fetched as un-extractable binary; `docs.espressif.com/projects/esp-datasheets/.../esp32-s3-wroom-1_wroom-1u_datasheet.html` returns 404. All dB pattern figures here are `[INFERRED]`.
- **Whether the board antenna is printed trace or ceramic chip, and its offset r from the spin axis.** Wiki says only "onboard patch antenna" (item ⑩). r is the single most important unknown in §1.1; resolvable from the PCB, not from search.
- **Abstracts of three prior-art papers** (10.1109/WPNC.2009.4907835; 10.1007/s11370-009-0046-7; 10.1109/JSEN.2011.2182046) — publisher-elided/paywalled. Titles, authors, venues verified; **no numbers attributed to them**.
- **Full text of Ubicarse (10.1145/2639108.2639142) and WSR (arXiv:2012.04174).** `people.csail.mit.edu/swarun/ubicarse.pdf` → 404; arXiv abs rendered title-only. The claim that both need translation + cross-antenna phase cancellation is `[DERIVED]`, not quoted.
- **Definitive list of consumer APs responding to FTM.** Mitigated by `wifi_ap_record_t.ftm_responder` in any scan.
- **Real-world back-to-back single-channel scan cadence on ESP32-S3.** The 35–50 ms figure is `[DERIVED]` from documented dwell plus event/`get_ap_records()` overhead, not measured.
- **Whether FTM initiator actually works in APSTA.** `esp_wifi.h` says "only in Station mode" but attention-2 references "internal SoftAP is started in". Contradictory; untested.
- **Grey literature.** DuckDuckGo HTML and Lite both returned block codes; Semantic Scholar and arXiv APIs returned HTTP 429 repeatedly. Search was done via Crossref REST, Semantic Scholar Graph and GitHub REST only. Hackaday/forum/YouTube builds are **not covered**.

## Adjacent, not pursued

- Bluetooth 5.1 AoA/AoD with CTE is the correct technology for RF bearing, but the ESP32-S3 BLE controller exposes no CTE or antenna-switching.
- GitHub search `esp32 "angle of arrival"` → 2 repos (acoustic GCC-PHAT, doppler radar); `wifi direction finding rssi` → **0 repos**. No established ESP32 WiFi-DF project exists.
- Non-RF absolute yaw (optical flow, ceiling-feature bearing, ToF ring to desk edges, a $2 I²C magnetometer) is 10–100× more accurate than anything RF here.
- ZUPT/ZARU is already exploited (29 updates / 48 s); per-temperature gyro bias tables are the natural next refinement once overshoot is fixed.
- CSI *motion/presence* sensing (`espressif/esp-csi` `esp-radar`) is a genuinely well-supported ESP32 capability and a far better use of the CSI pipeline than AoA.
- The MG90D differential drive's slip is a wheel-odometry problem, not a heading problem; the gyro already sidesteps it.

## References

### Load-bearing

1. `espressif/esp-idf:docs/en/api-guides/wifi.rst` (v5.3.1) — Wi-Fi Scan (120 ms dwell, 30 ms back-to-home, "supported only in station or station/AP mode", "concurrent scans should be avoided"); Wi-Fi Sniffer Mode (NULL/STA/AP/APSTA, "great impact" on throughput, callback in driver task); CSI (I/Q per sub-carrier, LTF byte table, `first_word_invalid`, CSD, "suggested to enable sniffer mode"); FTM (initiator STA / responder AP, "not accurate", prefer ESP↔ESP).
2. `espressif/esp-idf:components/esp_wifi/include/esp_wifi_types_generic.h` (v5.3.1) — scan-time defaults 0/120/360/30; `wifi_scan_config_t`; `wifi_ap_record_t` (`int8_t rssi`, `ftm_responder:1`, `ftm_initiator:1`); `beacon_interval` "1 TU = 1024 us … Default 100"; `wifi_ftm_initiator_cfg_t`.
3. `espressif/esp-idf:components/esp_wifi/include/local/esp_wifi_types_native.h` (v5.3.1) — `wifi_pkt_rx_ctrl_t` (rssi/noise_floor/µs timestamp/rate/sig_mode/channel), `wifi_promiscuous_pkt_t`, `wifi_csi_config_t`, `wifi_csi_info_t`.
4. `espressif/esp-idf:components/esp_wifi/include/esp_wifi.h` (v5.3.1) — `esp_wifi_ftm_initiate_session` attentions 1–2; `esp_wifi_ftm_resp_set_offset`; `esp_wifi_sta_get_rssi` ("last beacon", STA/APSTA).
5. `espressif/esp-idf:components/soc/esp32s3/include/soc/soc_caps.h` (v5.3.1) — `SOC_WIFI_FTM_SUPPORT (1)`, `SOC_WIFI_CSI_SUPPORT (1)`, `SOC_WIFI_HW_TSF (1)`.
6. `espressif/esp-idf:examples/wifi/ftm/README.md` (v5.3.1) — ESP32-S3 supported; "line-of-sight … no less than −70 dBm"; 49.218 ns → 5.07 m.
7. `espressif/esp-idf:docs/en/api-guides/phy.rst` (v5.3.1) — external antenna switch; "only a maximum of two antennas can be enabled simultaneously".
8. `espressif/esp-csi:README.md` §5 Note 1 — "PCB antenna has directivity".
9. `espressif/arduino-esp32:libraries/WiFi/src/WiFiScan.h:37-40`, `WiFiScan.cpp:52,69` — `max_ms_per_chan = 300`, `_scanActiveMinTime = 100`, `enableSTA(true)` promotes AP → AP_STA.
10. `waveshare.com/wiki/ESP32-S3-Touch-LCD-1.69` — "Onboard patch antenna" (⑩), 2.4 GHz 802.11 b/g/n.
11. `platformio.ini:23-44, 96-103`; `HARDWARE.md:24, 113` — ESP32-S3R8, espressif32@7.0.1, softAP WebSocket joystick stack.
12. Kotaru, Joshi, Bharadia, Katti, *SpotFi*, SIGCOMM 2015 — DOI 10.1145/2785956.2787487.
13. Ledergerber & D'Andrea, Sensors 19(20):4466, 2019 — DOI 10.3390/s19204466.
14. *Dual-Antenna Mobile Robot Orientation Estimation … RSSI Fingerprinting*, Acta Mech. et Autom. 2025 — DOI 10.2478/ama-2025-0079 (MCC 0.23, F1 0.39).
15. Zhou, He, Krishnamurthy, Stankovic, RIM, ACM TOSN 2006 — DOI 10.1145/1149283.1149287.
16. Android Developers, *Wi-Fi RTT* — "typically accurate within 1-2 meters"; AP must implement 802.11-2016 FTM.

### Background

17. Graefenstein, Albert, Biber, Schilling, WPNC 2009 — DOI 10.1109/WPNC.2009.4907835 (rotating antenna on a mobile robot; abstract elided).
18. Yi, Kim, Song, Intell. Serv. Robotics 2009 — DOI 10.1007/s11370-009-0046-7 (directional antenna on robot).
19. Malajner, Planinšič, Gleich, IEEE Sensors J. 2012 — DOI 10.1109/JSEN.2011.2182046 (AoA from RSSI, rotatable omni antennas).
20. Kumar, Gil, Katabi, Rus, *Ubicarse*, MobiCom 2014 — DOI 10.1145/2639108.2639142.
21. *A wireless signal-based sensing framework for robotics* (WSR) — arXiv:2012.04174.
22. Lymberopoulos, Lindsey, Savvides, EWSN 2006 — DOI 10.1007/11669463_24.
23. `StevenMHernandez/ESP32-CSI-Tool` (537★, MIT); `Gi-z/CSIKit`; `espressif/esp-csi` `esp-radar` — all sensing, none AoA.
