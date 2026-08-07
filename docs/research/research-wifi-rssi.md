# Research: WiFi RSSI as a Position/Heading Reference

## Executive summary (max 12 bullets, each tagged)

1. **The user's exact idea has been done, has a name, and its canonical result explicitly cannot produce absolute heading.** WiFi-SLAM (Ferris, Fox & Lawrence, IJCAI-07) maps signal strength with *no* known AP positions and no location labels — but the recovered map lives in a GP-LVM latent space, and the authors state outright: *"the mapping from the latent coordinate system X to the underlying ground truth coordinate system is not well-defined."* **[SOURCED]** (ijcai.org/Proceedings/07/Papers/399.pdf, §5)
2. **WiFi-SLAM's accuracy: mean localisation error 3.97 ± 0.59 m**, from traces 250–500 m long, across a 60 × 50 m building with three loop closures, and it required strong structural priors (walking-speed prior, straight-hallway prior, parallel-path alignment prior). **[SOURCED]** (ibid., §4–5)
3. **RSS fingerprinting accuracy in real systems is 0.6–7.6 m median; RSS ranging ~2.4 m.** Sub-metre results only occur with ~100 APs visible, or in atypically favourable testbeds. **[SOURCED]** (Leitch et al., Sensors 2023, Table 2; Kolakowski, Sensors 2021, Table 1)
4. **At 2.4 GHz the RF field decorrelates in ~4.7 cm** (Clarke/Jakes isotropic-scattering first zero at 0.38 λ, λ ≈ 12.3 cm). Rayleigh fading gives **σ = 5.57 dB in the log domain**, with a 9.5% chance of a >10 dB fade and 1% chance of >20 dB at any given point. **[DERIVED]** from standard channel theory
5. **The desk-scale signal is smaller than one LSB.** Driving a 30 cm leg 5 m from an AP with path-loss exponent n = 2 changes true RSS by **0.52 dB**. The ESP32 reports RSSI as `int8_t` — **1 dBm quantisation**. The signal is *half a quantisation step*, before multipath. **[DERIVED]** + **[SOURCED]** (esp-idf `esp_wifi_types_generic.h`, `wifi_ap_record_t.rssi` is `int8_t`)
6. **Multipath error is a repeatable bias, not averageable noise.** In a static room the fading pattern is a deterministic function of position; re-driving the same square re-samples the same fades. Time-averaging removes receiver noise, not multipath. **[DERIVED]**
7. **Quantified heading capability of the survey-by-driving approach: a full 1 m² raster at 5 cm spacing (≈400 stops, tens of minutes) yields a per-AP bearing of only ~±37° (1σ).** A 30 cm square yields essentially nothing. **[DERIVED]**
8. **Heading via position fixes is worse.** To resolve heading to ±5° from RSSI position fixes with ±3 m error requires a ~50 m straight baseline; even with an optimistic ±0.6 m fingerprint fix you need ~10 m for ±5°, ~1.7 m for ±30°. A desk robot never travels that far in a straight line. **[DERIVED]**
9. **Path-loss exponent is not a constant even inside one building:** γ = 4.2 in an office room vs γ = 1.2 in a corridor at 2.4 GHz. "Stronger = closer" is not reliably true indoors. **[SOURCED]** (Retscher, Sensors 2020, Table 4)
10. **Fingerprint decay is a recognised, unsolved problem** — a 15-month, multi-orientation RSS dataset exists specifically because fingerprinting "faces two major problems: lack of robustness to short and long time signal changes." **[SOURCED]** (Mendoza-Silva et al., *Data* 3(1):3, 2018)
11. **Robotic RSS-gradient seeking is real but coarse:** Twigg/Fink/Yu/Sadler (ICRA 2012) estimate a 2-D RSS gradient from local motion to *prioritise frontiers* during exploration — i.e., to bias a search, not to fix a pose. **[SOURCED]** (zenodo.org/records/1271335)
12. **Verdict: (d) for heading, (b) for coarse place/room-level.** WiFi RSSI is not a viable heading reference for this robot at any survey size it can physically execute. It *is* a cheap, robust room-level place recogniser and kidnap detector. A 30-minute **stationary** scan-logging test settles the question without writing any motion code. **[DERIVED]**

---

## 1. Prior art (WiFi-SLAM, fingerprinting, source seeking, RSSI+odometry)

### 1.1 WiFi-SLAM — Ferris, Fox & Lawrence, IJCAI 2007 (this *is* the user's idea)

Full text: https://www.ijcai.org/Proceedings/07/Papers/399.pdf (pp. 2480–2485). Bibliographic record confirming authorship/venue/pages: https://inverseprobability.com/publications/ferris-wifi07.html

**What it does.** It solves exactly the problem the user posed: build a spatial representation from raw, *unlabelled* signal-strength sequences, without knowing where the APs are. It maps a *d*-dimensional RSS vector (one dimension per AP) into a 2-D latent space using a Gaussian Process Latent Variable Model. Three constraints are imposed **[SOURCED]** (§1):

1. *Locations → signal strength*: nearby locations have similar RSS (the GP kernel).
2. *Signal strengths → locations*: similar RSS ⇒ nearby locations — "extremely important to close loops." The paper explicitly flags this as approximate: *"While this constraint might be violated for individual access points, it is a good approximation for typical office environments, where multiple access points are visible at each point in time."*
3. *Locations → locations*: consecutive readings are near each other (motion model).

The motion prior `p(X) = p(distance)·p(orientation)·p(alignment)` (Eq. 14) encodes a Gaussian walking-velocity prior, a zero-mean prior on heading change (i.e. "mostly walk straight"), and an alignment term that pulls near-parallel path segments together within 5 m. **[SOURCED]** (§4, Eqs. 15–17)

**What it achieves.**
- Topologically correct connectivity graphs and a GP sensor model, from three traces of 250 m to 500 m over one floor of a 60 × 50 m university building, with three loop closures and multiple passes per hallway. **[SOURCED]** (§5)
- **Mean localisation error 3.97 ± 0.59 m** over six cross-validation runs. **[SOURCED]** (§5)
- Isomap initialisation was required to avoid local minima; conjugate-gradient descent on the negative log-likelihood. **[SOURCED]** (§4)

**The three facts that matter most for this robot** (all **[SOURCED]**, §5):
- *No absolute orientation.* "Since the mapping from the latent coordinate system X to the underlying ground truth coordinate system is not well-defined, we evaluated localization accuracy using the metric described in [Bowling et al., 2005]." The evaluation is reduced to *"whenever a person returns to a previously visited location, we estimate how accurately our system can detect this event."* The output is **place recognition, not pose**.
- *Geometry is hallucinated from priors.* "The ground truth trace includes a non-right-angle intersection, which is simplified as a right angle in our GP-LVM result… the constraint emphasizing straight and aligned paths tends to prefer a grid like structure." The map's shape came substantially from the office-building prior, not from the RF.
- *Ranging was rejected as infeasible.* "In principle we could extract distance to access point estimates from signal strength measurements and then apply range-only SLAM… However, such techniques typically rely on rather accurate range measurements, which are clearly not available in our context." **[SOURCED]** (§2)

Note also §2: *"even when considering the location and material of walls and furniture inside buildings, the accuracy of signal propagation models is very limited."*

### 1.2 RSSI fingerprinting: RADAR, Horus, and the general approach

- **RADAR** (Bahl & Padmanabhan, IEEE INFOCOM 2000; DOI 10.1109/infcom.2000.832252; 8,366 citations). Records RSS at multiple base stations, combines empirical measurements with propagation modelling. The published abstract claims only "a high degree of accuracy" without a number. **[SOURCED]** (OpenAlex W2170102584 abstract). The widely-quoted "median 2–3 m" figure is **[SOURCED, secondary]** — I could not retrieve the primary PDF (see *Could not verify*).
- **Horus** (Youssef & Agrawala, MobiSys 2005; DOI 10.1145/1067170.1067193; 1,654 citations). Abstract: *"It has an error less than 0.6 meter on average and its computational requirements are more than an order of magnitude better than other systems."* Achieved by explicitly modelling "different causes for wireless channel variations" plus location clustering. **[SOURCED]** (OpenAlex W2051376734 abstract)
- **Survey effort is the defining cost.** Kolakowski: *"The most significant disadvantage of fingerprinting is the effort required to construct the radio map… the performance of the method is dependent on the map density and a number of conducted measurements… Additionally, the radio maps often become outdated when even small changes are introduced into the system environment (e.g., moving an access point or placing additional pieces of furniture)."* **[SOURCED]** (Sensors 21(18):6270, §1 — https://pmc.ncbi.nlm.nih.gov/articles/PMC8473432/)
- **Reported accuracy ranges (peer-reviewed comparison tables):**

  | Technique | Reported accuracy | Source |
  |---|---|---|
  | WiFi RSS ranging | 2.397 m | Leitch et al., *Sensors* 23(20):8598, Table 2 **[SOURCED]** |
  | WiFi RSS fingerprinting | 0.169 – 7.6 m | ibid., Table 2 **[SOURCED]** |
  | WiFi CSI fingerprinting | 0.09 – 2.087 m | ibid., Table 2 **[SOURCED]** |
  | WiFi RTT (802.11mc FTM) ranging | 0.5 – 2.10 m | ibid., Table 2 **[SOURCED]** |
  | WiFi fingerprinting, 12 systems | 0.60 – 4.3 m median | Kolakowski, *Sensors* 21(18):6270, Table 1 **[SOURCED]** |

  Kolakowski's own conclusion: *"The typical median localisation error of the RSS-based fingerprinting methods is in the range of 1–2 m."* And on the outliers: sub-metre WiFi results occurred only *"where either the number of the anchors was very high (100 available access points—21 best chosen for localisation) or the test conditions were very specific (large area partitioned into smaller rooms using low drywall walls)."* **[SOURCED]**

- Leitch et al.'s summary of RSS ranging's disadvantage is a one-liner worth quoting: *"RSS values fluctuate when stationary."* **[SOURCED]** (Table 2)

### 1.3 Robotic radio-source seeking / gradient following

- **Twigg, Fink, Yu & Sadler, "RSS gradient-assisted frontier exploration and radio source localization," ICRA 2012** (DOI 10.1109/icra.2012.6225059; open copy at https://zenodo.org/records/1271335). *"We incorporate radio signal strength (RSS) information into the exploration algorithm by locally sampling the RSS and estimating the 2-D RSS gradient. The algorithm exploits the local motion to collect RSS samples for gradient estimation and seeks to explore in a way that brings the robot to the signal source."* **[SOURCED]** This is the closest published match to "drive a pattern, estimate a local gradient." Note the use: the gradient **dynamically prioritises candidate frontiers** in an exploration planner — a search heuristic. It is not used as a pose measurement, and the robot traverses a *complex indoor environment* (building scale), not a desk. **[SOURCED]** (abstract) / **[INFERRED]** on scale.
- **Fink & Kumar, "Online methods for radio signal mapping with mobile robots," ICRA 2010** (DOI 10.1109/robot.2010.5509574, 108 citations). Confirmed to exist; full text not retrievable (see *Could not verify*). **[SOURCED]** (OpenAlex W2023292064, metadata only)

### 1.4 RSSI + odometry fusion; RSSI as loop closure in graph SLAM

- **Huang, Millman, Quigley, Stavens, Thrun & Aggarwal, "Efficient, generalized indoor WiFi GraphSLAM," ICRA 2011**, pp. 1038–1043 (DOI 10.1109/icra.2011.5979643, 218 citations, top-1% cited for its year). Confirmed to exist; full text not retrievable. **[SOURCED]** (OpenAlex W2105061875, metadata only)
- Kolakowski's related-work survey documents the standard patterns **[SOURCED]** (*Sensors* 21(18):6270, §2):
  - *"the location of the fingerprints is derived with a dead-reckoning based GraphSLAM. The novelty… is a loop-closure achieved using magnetic field measurements and observed Wi-Fi signals similarity."*
  - *"the proposed solution introduces a concept of so-called 'Virtual Wi-Fi landmarks' located at the typical trajectory turns."* — reported median error 4.3 m in Table 1, i.e., WiFi-as-graph-constraint is the *worst* entry in the table.
  - Gaussian Process Regression is the dominant interpolator for robot-collected RSS.
  Interpretation: **in every one of these systems WiFi supplies loop closure / place identity, and an odometry or LiDAR front-end supplies the metric geometry.** WiFi is never the metric or angular reference. **[DERIVED]** from the table + method descriptions.

### 1.5 A robot surveying by driving a pattern

- **Kolakowski, "Automated Calibration of RSS Fingerprinting Based Systems Using a Mobile Robot and Machine Learning," *Sensors* 21(18):6270, 2021** (https://pmc.ncbi.nlm.nih.gov/articles/PMC8473432/) is the closest published instance of "let the robot drive around and build the signal map." **[SOURCED]**
  - Method: LiDAR + GraphSLAM builds the occupancy map first; then *"The RSS radio map calibration consists of driving the platform through the system deployment area. At the same time, the robot localises itself using the LiDAR and concurrently measures the strengths of signals."* **[SOURCED]** (§3)
  - Fingerprints are **binned on a 0.1 m grid and averaged** before model fitting. **[SOURCED]** (§5)
  - Result in a fully furnished apartment: **median robot positioning error 0.87 m** (neural-net regression); 0.4 m median trajectory error for a walking person. **[SOURCED]** (Abstract)
  - **The direction of information flow is the opposite of the user's proposal:** the robot needs an *independent* metric localiser (LiDAR + SLAM) to label the RSS samples. The RF does not supply the geometry; it consumes it. **[DERIVED]**
- Retscher documents the survey-cost reduction trick — measuring only along trajectories instead of a regular grid — *"Then, the workload can be reduced by a factor of 4"*, with *"accuracies… similar to static training measurements at waypoints while saving a lot of time."* **[SOURCED]** (*Sensors* 20(18):5121, §3.1.1)

---

## 2. How RSSI actually behaves (physics and practical reality)

### 2.1 Log-distance path loss and its indoor validity

The standard one-slope model is `P = P₀ − 10γ·log₁₀(d)` **[SOURCED]** (Retscher, *Sensors* 20(18):5121, §3.1.1), or with shadowing, `PL(d) = PL₀ + 10n·log₁₀(d/d₀) + S_σ`, `S_σ ~ logN(0, σ²)` **[SOURCED]** (Hailu et al., *Sensors* 24(21):6876, §2.2, Eq. 1).

The model's indoor validity is poor, and the literature says so plainly:

| Evidence | Value | Tag |
|---|---|---|
| γ, office building, 2.4 GHz | **4.2** | **[SOURCED]** Retscher Table 4 |
| γ, corridor, **same band** | **1.2** | **[SOURCED]** Retscher Table 4 |
| γ, office building, 5 GHz | 4.6 | **[SOURCED]** Retscher Table 4 |

A 3.5× spread in the exponent *within the same building* means the RSS→distance map is not a function; it depends on which way the propagation happens to be going. **[DERIVED]**

Supporting statements:
- *"The damping factor γ can vary significantly as indicated, which can then cause large errors for RSS to range conversions."* **[SOURCED]** (Retscher §3.1.1)
- *"even when considering the location and material of walls and furniture inside buildings, the accuracy of signal propagation models is very limited."* **[SOURCED]** (Ferris et al. §2)
- *"model-based RSS approaches… suffer from significant limitations due to the large fluctuations in RSS values in complex indoor environments."* **[SOURCED]** (Hailu et al. §2.2)
- Even the multi-wall model fails: *"in very complex environments, the multi-wall model did not outperform the one-slope model. The main reason… Multipath, refraction, and diffraction are out of consideration, as they cannot be modeled easily in practice."* **[SOURCED]** (Retscher §3.1.1)
- Ferris et al. Fig. 1 (GP model of one AP over a 60×50 m floor) *"clearly demonstrates that signal strength propagation cannot be represented adequately by a parametric sensor model such as one that estimates the location of an access point and then assumes a radial signal propagation model."* **[SOURCED]** (§3.2)

**Consequence for the user's premise "if the signal gets stronger you know you're getting closer":** this is true on average over many metres and many wavelengths. It is not reliably true over the tens of centimetres a desk robot moves. **[DERIVED]**

### 2.2 Multipath and fast fading — quantified at 2.4 GHz

**Wavelength.** λ = c/f. Channel 1 (2.412 GHz) → 12.43 cm; channel 6 (2.437 GHz) → 12.31 cm; channel 11 (2.462 GHz) → 12.18 cm. **λ ≈ 12.2–12.4 cm; λ/2 ≈ 6.1–6.2 cm.** **[DERIVED]**

**Decorrelation distance.** Under Clarke's isotropic-scattering model the spatial autocorrelation of the complex field is `J₀(2πd/λ)`. The first zero of J₀ is at argument 2.405, giving `d = 0.3827 λ` = **4.7 cm at 2.4 GHz**. Two RSSI readings taken ~5 cm apart are, to first order, *statistically independent draws*, not two points on a smooth curve. **[DERIVED]** from standard mobile-radio channel theory.

**Fade depth.** For Rayleigh fading the instantaneous power is exponentially distributed. In the log domain:
- **Standard deviation = (10/ln10)·π/√6 = 5.57 dB** **[DERIVED]**
- Mean of 10·log₁₀(power) sits 2.51 dB *below* 10·log₁₀(mean power) **[DERIVED]**
- P(fade deeper than 10 dB) = 1 − e^(−0.1) = **9.5%** **[DERIVED]**
- P(fade deeper than 20 dB) = 1 − e^(−0.01) = **1.0%** **[DERIVED]**

So: **at 2.4 GHz, moving ~5 cm can plausibly change RSSI by 10 dB or more, and roughly 1 point in 100 sits in a >20 dB null.** In practice indoor channels are Rician (there is often a dominant path), which reduces σ below 5.57 dB, but the desk environment — metal desk legs, monitor stands, laptop, chair — is closer to the rich-scattering end. **[DERIVED]** / Rician mitigation **[INFERRED]**

**The critical corollary: this is not noise you can average away.** For a static room and a static AP, the multipath field is a deterministic function of position. Taking 100 samples while parked at one spot averages out receiver thermal noise and interference; it does **not** average out the fade, because the fade is the same every sample. Re-driving the same 30 cm square ten times re-visits the same four fades. Only *new positions*, spaced ≥ ~5 cm apart and covering many wavelengths, add information. **[DERIVED]**

Independent corroboration from the survey literature: Hailu et al. Figure 2 shows *"RSS measurements from a Wi-Fi access point exhibit a wide range of signal values at various reference points… At target 1, for example, there are extreme signal fluctuations."* **[SOURCED]** (*Sensors* 24(21):6876, §2.2). Retscher describes his own radio maps as showing *"large differences between the minimum and maximum RSS values"* attributable to *"high fluctuations of the RSS."* **[SOURCED]** (§3.1.1, Fig. 8)

### 2.3 Shadowing: robot body, furniture, people

- Retscher's radio maps are built as the *"median, arithmetic mean, minimum, and maximum RSS of four orientations"* at each reference point — the survey protocol treats **receiver orientation as a first-class nuisance variable** that must be sampled, exactly because the body/device blocks the signal. **[SOURCED]** (*Sensors* 20(18):5121, Fig. 8 caption)
- The 15-month benchmark dataset was collected with *"a trained professional… standing at specific positions and facing specific directions"*, with orientation recorded per fingerprint. **[SOURCED]** (Mendoza-Silva et al., *Data* 3(1):3, abstract)
- Kolakowski's motivation for testing in a furnished apartment: *"In most works, the experiments are performed in office spaces, where the nature of the received power changes is different than in smaller, cluttered environments."* A desk is the extreme cluttered case. **[SOURCED]** (§1)
- The ESP32-S3 has a single PCB antenna; the robot's own battery, motors and PCB ground plane sit between the antenna and roughly half the azimuth. Azimuthal gain variation of several dB with nulls is normal for PCB IFA/meander antennas. **[INFERRED]** — not verified against a measured pattern for this specific board.
- One published mitigation worth knowing: derived features that cancel receiver-side effects — **Signal Strength Difference (SSD)**, **Hyperbolic Location Fingerprint (HLF)**, **Difference of Fingerprint (DIFF)** — i.e., use RSS(AP_i) − RSS(AP_j) rather than absolute RSS. **[SOURCED]** (Hailu et al., *Sensors* 24(21):6876, §2.2)

### 2.4 AP-side variation

- **Beacon interval is 100 ms by default** and configurable 100–60000 ms. **[SOURCED]** (ESP-IDF v5.3 Wi-Fi Driver guide, SoftAP config table). This sets the fastest possible passive sampling rate per AP and the minimum useful per-channel dwell.
- **Transmit power control, auto-RF and band steering are standard in modern consumer/enterprise APs** (802.11h TPC, 802.11k RRM, vendor auto-RF). Mesh systems (eero/Nest/Deco) change channel and power autonomously. A "fixed reference point" that silently retunes its transmit power invalidates absolute RSSI. **[INFERRED]** — mechanism is standard, but I did not verify a specific measurement study of the magnitude.
- **Beamforming is a smaller threat than it looks.** Beacons and probe responses are broadcast at a basic rate to unknown receivers and are therefore generally *not* beamformed, so passive beacon RSSI is less affected than data-frame RSSI. **[INFERRED]**
- **Multiple SSIDs on one radio are not independent references.** A router advertising main + guest + IoT SSIDs presents 3 BSSIDs from one physical antenna at one location. Counting them as 3 "different directions" is a modelling error. **[DERIVED]**
- **2.4-GHz-only reception halves the visible population.** Dual-band APs sharing an SSID expose only their 2.4 GHz radio to this robot; 5/6 GHz neighbours are invisible. **[DERIVED]** from the stated hardware constraint.
- The surveys name AP-side and device-side variability as first-order error sources: *"its performance can be affected by temporal variations in RSS, device heterogeneity, and fingerprint duplication."* **[SOURCED]** (Hailu et al. §2.2)

### 2.5 Temporal stability

- The existence and framing of the UJI long-term dataset is the cleanest evidence: *"WiFi fingerprinting… currently faces two major problems: lack of robustness to short and long time signal changes and difficult reproducibility… The data collection spanned over 15 months, and, for each month, one type of training datasets and five types of test datasets were collected."* **[SOURCED]** (Mendoza-Silva, Richter, Torres-Sospedra, Lohan & Huerta, *Data* 3(1):3, 2018, DOI 10.3390/data3010003, CC-BY)
- *"the radio maps often become outdated when even small changes are introduced into the system environment (e.g., moving an access point or placing additional pieces of furniture). Therefore, maintaining high localisation accuracy would require frequent calibration."* **[SOURCED]** (Kolakowski §1)
- An entire sub-literature exists on *detecting* AP changes and repairing maps: *"Gradient Boosted Decision Tree regression is used to detect access points alterations and update the fingerprint database"* (2.1 m median). **[SOURCED]** (Kolakowski §2, Table 1)
- I did **not** find a primary paper quantifying "error after N weeks" — see *Could not verify*. The direction is unambiguous; the magnitude is not established here.

---

## 3. Achievable spatial resolution; absolute vs gradient

### 3.1 What real systems get

Consolidating §1.2: **RSS fingerprinting indoors lands at 1–2 m median typically, 0.6 m best-case with ~100 APs and a dense manual survey, 4–8 m worst case.** RSS ranging without fingerprints: ~2.4 m. Unsupervised/SLAM-style (the user's approach): 4.0 m (Ferris) to 4.3 m (WiFi virtual landmarks in GraphSLAM). **[SOURCED]**

**The robot's entire workspace is smaller than the best-case error bar.** A desk robot operating in ~1 m² receives, from a best-in-class WiFi fingerprint system, a position estimate whose 1σ uncertainty exceeds the workspace diameter. **[DERIVED]**

### 3.2 Is a gradient more robust than an absolute value?

**Yes — and it does not save the idea.** The gradient is more robust in three specific ways, all real:

1. It cancels unknown TX power and unknown AP-specific offsets (they're constants; d/dx of a constant is zero). **[DERIVED]**
2. It cancels slowly-drifting receiver gain and, to first order, robot-body shadowing that stays fixed in the body frame. **[DERIVED]**
3. It cancels the absolute-level part of temporal drift (AP TPC changes the level, not the local slope, over short survey windows). **[DERIVED]**

But it makes the *noise* problem strictly worse, because differencing two noisy quantities amplifies noise by √2 while the signal shrinks with the baseline. The numbers:

**Gradient magnitude.** From `RSS(d) = C − 10n·log₁₀ d`:
`|∇RSS| = 10n/(d·ln10) = 4.343·n/d` dB per metre. **[DERIVED]**

| Distance to AP | n = 1.2 (corridor) | n = 2 (free-space-ish) | n = 4.2 (office) |
|---|---|---|---|
| 2 m | 2.61 dB/m | 4.34 dB/m | 9.12 dB/m |
| 5 m | 1.04 dB/m | 1.74 dB/m | 3.65 dB/m |
| 10 m | 0.52 dB/m | 0.87 dB/m | 1.82 dB/m |

**Displacement needed to exceed the noise floor.** Two noise floors apply:

- **Quantisation floor: 1 dB.** ESP-IDF exposes `int8_t rssi` in `wifi_ap_record_t`. **[SOURCED]** (esp-idf v5.3 `components/esp_wifi/include/esp_wifi_types_generic.h`). To move RSS by ≥1 dB at 5 m / n = 2 you must travel **0.57 m**. At 10 m / n = 1.2, **1.9 m**. **[DERIVED]**
- **Fading floor: σ ≈ 5.6 dB per independent sample.** To make a single-sample RSS difference exceed 1σ of fading (`Δ > √2·σ = 7.9 dB`) at 5 m / n = 2 you must travel **4.5 m**. **[DERIVED]**

**Summary answer to "how much displacement before RSSI change exceeds the noise floor":** for a *single* pair of readings, **metres, not centimetres**. Sub-metre gradient extraction is only possible by averaging many spatially-independent samples, which is what §5 quantifies.

---

## 4. Can RSSI constrain HEADING?

*(A companion agent is covering rotational / synthetic-aperture bearing estimation. This section deliberately restricts itself to whether the SURVEY/MAP approach yields heading.)*

### 4.1 The structural answer, from the primary source

**No, not on its own — and this is proved by the canonical paper rather than argued.** GP-LVM places a zero-mean unit-variance Gaussian prior on the latent coordinates: `p(x_ij) = N(x_ij; 0, 1)` **[SOURCED]** (Ferris et al. §3.3). That prior is rotationally symmetric, and the GP kernel `k(x_i, x_j) = σ_f² exp(−|x_i − x_j|²/2l²)` depends only on **distances**, not on absolute directions **[SOURCED]** (Ferris et al. Eq. 2). Any global rotation or reflection of the latent map is an equal-likelihood solution.

The authors say so operationally: *"Since the mapping from the latent coordinate system X to the underlying ground truth coordinate system is not well-defined…"* **[SOURCED]** (§5)

**A map built from RSS + distances is determined only up to a rigid rotation.** Absolute heading is exactly the quantity that rotation destroys. Building the map does not recover it; you need one external anchor. **[DERIVED]**

Ferris et al. reach the same conclusion in their future work: *"we wish to integrate additional sensors, such as accelerometer, **digital compass**, and barometer, to increase the accuracy of our technique."* The state-of-the-art WiFi-SLAM paper's own answer to the heading problem is **add a magnetometer**. **[SOURCED]** (§6)

### 4.2 Does RSSI carry bearing information at all?

- **From a single omnidirectional antenna at a single point: no.** RSSI is a scalar. There is no direction in a scalar. **[DERIVED]**
- **From a spatial gradient of the scalar field: yes, in principle.** ∇RSS is a vector. Under a monotone path-loss model with no multipath, ∇RSS points at the AP. Estimated in the robot's **body frame** (using the IMU to relate survey legs) and compared against a stored **world-frame** bearing for that AP, it would give heading. This is the strongest form of the user's idea, and it is the mechanism Twigg et al. use — *"locally sampling the RSS and estimating the 2-D RSS gradient"* **[SOURCED]** — though they use it to steer exploration, not to fix pose.
- **Where the literature actually gets bearing:** antenna arrays with phase (MUSIC/AoA — ArrayTrack, SpotFi, LTEye), or synthetic apertures from motion, or directional antennas. All of these need **phase** or **CSI**, not RSSI. **[SOURCED]** (Wikipedia, *Wi-Fi positioning system*, "Angle of arrival" section, citing ArrayTrack/SpotFi/LTEye). Leitch et al. classify "Hybrid AoA/Ranging" separately from RSS and note it *"Requires specific hardware"* **[SOURCED]** (Table 2). The ESP32-S3 with a single PCB antenna and no CSI-phase pipeline in this firmware has none of this. **[DERIVED]**
- **Why the multipath problem is qualitatively worse for bearing than for position.** The bearing error induced by multipath is the *angle* between ∇RSS(true field) and the true AP direction. In a room with a strong reflector (wall, monitor, filing cabinet) the local field gradient can point 90° or 180° away from the AP, and it does so **consistently at that spot**. Repeating the survey reproduces the error. This is a bias, not noise. **[DERIVED]**
- One genuinely useful nuance: because the bias is *repeatable*, a survey done at the robot's dock could serve as a **place-specific heading fiducial** ("at the dock, RSSI vector V means yaw ≈ θ"), calibrated once against a known heading. But that (a) is a lookup table, not a measurement, (b) requires returning to the same few-cm patch, (c) breaks when the room changes, and (d) leans on yaw-dependence of RSSI at a fixed point — which is the rotational/antenna-pattern mechanism the companion agent owns. Flagged, not pursued here.

### 4.3 Heading from a sequence of RSSI *position* fixes

The other route: if RSSI gives position, two position fixes separated by baseline B give a heading. Error propagation: `σ_θ ≈ σ_p·√2 / B` (σ_p = per-axis position error). **[DERIVED]**

| σ_p (position fix quality) | B for ±30° | B for ±5° |
|---|---|---|
| 3.0 m (Ferris-class WiFi-SLAM, 3.97 m mean) | 8.1 m | 48.7 m |
| 2.0 m (typical fingerprinting) | 5.4 m | 32.5 m |
| 0.6 m (Horus-class, dense manual survey) | 1.6 m | 9.7 m |

**A robot that "moves in short bursts" on a desk cannot generate any of these baselines.** Even the most optimistic row requires travelling 1.6 m in a straight line to get ±30°. **[DERIVED]**

---

## 5. Assessment of the survey-by-driving approach

### 5.1 What trajectory shapes the literature uses, and how long surveys take

- **Grids of static reference points** (classical fingerprinting): *"the radio map is created manually by taking multiple measurements in the area covered by the system… may be a lengthy and tiresome process."* **[SOURCED]** (Kolakowski §1)
- **Trajectories rather than grids**, to cut cost by 4× **[SOURCED]** (Retscher §3.1.1)
- **Robot driving the whole area with LiDAR localisation**, then binning fingerprints on a **0.1 m grid** and averaging **[SOURCED]** (Kolakowski §3, §5)
- **Human walking traces of 250–500 m** through a 60 × 50 m building, with three loop closures and multiple passes per hallway — for a 3.97 m result **[SOURCED]** (Ferris et al. §5)

**Every published survey covers the whole *building*, at *metre* spacing, over *hundreds of metres* of travel.** Nobody surveys a 30 cm square, because 30 cm is 2.4 wavelengths and the resulting differences are inside the fade structure. **[DERIVED]**

### 5.2 The quantitative verdict on a desk-sized survey

Setup: the robot drives a square of side L, takes N spatially-independent RSSI samples per side, and fits a plane to estimate ∇RSS. Two opposite sides give one component; the other two give the orthogonal component.

- Signal per axis: `ΔRSS = |∇RSS| · L`
- Noise on that difference: `σ·√(2/N)`
- Resulting angular 1σ: `σ_θ ≈ σ·√(2/N) / (|∇RSS|·L)` radians **[DERIVED]**

Using σ = 5.57 dB (Rayleigh), AP at 5 m, n = 2 → |∇RSS| = 1.74 dB/m:

| Survey square side L | ΔRSS (true signal) | Independent samples per side needed for σ_θ = 30° | Travel implied at 5 cm sample spacing |
|---|---|---|---|
| **0.30 m** (the user's proposal) | **0.52 dB** | **840** | **42 m per axis** |
| 0.30 m | 0.52 dB | 3,361 (for σ_θ = 15°) | 168 m per axis |
| 1.00 m | 1.74 dB | 76 | 3.8 m per axis |
| 1.00 m, full raster (400 stops) | 1.74 dB | N ≈ 200/side ⇒ **σ_θ ≈ 37°** | ~20 m of driving, 400 scans |

**[DERIVED]** — all rows.

Three things to notice:

1. **The 30 cm square's true signal is 0.52 dB — below the ESP32's 1 dB RSSI quantisation step.** The measurement cannot represent it even with zero multipath. **[DERIVED]** + **[SOURCED]** (`int8_t rssi`)
2. **You cannot buy the required N by repeating.** N must be *spatially independent* samples ≥ ~5 cm apart. A 30 cm square perimeter (1.2 m) holds at most ~24 independent positions total, ~6 per side. Repeating the square 100 times gives 100 correlated copies of those same 6. **[DERIVED]**
3. **The best physically achievable desk survey — a complete 1 m² raster at 5 cm spacing, ~400 stops — yields a per-AP bearing of roughly ±37° (1σ), and that estimate is optimistic** because it treats the fading as zero-mean i.i.d. It is not: the shadowing field has its own spatial structure that adds a repeatable bias. **[DERIVED]**

Compare against the robot's existing performance: **heading holds ±0.05° over 48 s stationary; a 360° turn takes 1260 ms and is accurate under gyro closed loop.** A ±37° WiFi bearing that costs 400 measurement stops is roughly **three orders of magnitude worse than the sensor it is meant to correct**, and — being a repeatable bias rather than zero-mean noise — it will not average down over repeated fixes. **[DERIVED]**

### 5.3 Practical costs on *this* hardware

- **Firmware is `WIFI_AP`-only, channel 6 pinned.** `src/main.cpp:6996` sets `WiFi.mode(WIFI_AP)`; `src/main.cpp:3426` pins `kOverrideApChannel = 6`; `src/main.cpp:7004` starts the SoftAP. **[SOURCED]** (repo `src/main.cpp`)
- **You cannot scan in AP-only mode.** Adding station-mode scanning means `WIFI_AP_STA` and off-channel hopping. **[DERIVED]**
- **Scanning disrupts the control link.** ESP-IDF explicitly notes *"the timer used for beacon timeout will be reset during the scanning process. It means that the scan process will affect the triggering of the event WIFI_EVENT_STA_BEACON_TIMEOUT."* **[SOURCED]** (ESP-IDF v5.3 Wi-Fi Driver, "Wi-Fi Beacon Timeout"). For the SoftAP side, hopping away from channel 6 means missed beacons and stalled WebSocket traffic to the phone joystick — which is the robot's control path, and which the firmware already treats as safety-critical (`resetDriveControl("WiFi transport loss")`, `src/main.cpp:6443-6444`). **[SOURCED]** on the code; **[INFERRED]** on the magnitude of disruption.
- **Scan latency.** A full 2.4 GHz passive scan must dwell ≥ 1 beacon interval per channel; ESP-IDF's default beacon interval is **100 ms** **[SOURCED]** (SoftAP config table), so ≥ **1.3 s** for 13 channels, before per-channel overhead. **[DERIVED]** Exact ESP-IDF default active/passive dwell values not verified (see *Could not verify*). At ~1.5 s per scan and 400 stops, a 1 m² raster survey costs **10+ minutes of scanning alone**, plus drive-and-settle time, per survey — and it must be repeated whenever heading is needed.

### 5.4 Would a desk-sized robot moving tens of centimetres get anything usable?

**For heading: no.** 0.52 dB of signal, 1 dB of quantisation, 5.6 dB of fading, ~24 available independent sample positions against the ~840–3,400 required.

**For "am I closer or further from the AP" over tens of centimetres: no.** Same numbers.

**For "have I been moved to a different part of the room / a different room": yes, comfortably.** Room-to-room RSSI vector changes are tens of dB across multiple APs, far above every noise floor listed. This is precisely the constraint Ferris et al. rely on for loop closure. **[SOURCED]** (§1, constraint 2) / **[DERIVED]**

---

## 6. VERDICT for this robot (with the minimum experiment that would settle it)

### 6.1 The verdict

**(d) for heading — not worth the complexity. (b) for coarse, room-level place recognition — genuinely useful and cheap.**

Explicitly against each option:

- **(a) Viable absolute heading reference — NO.** Three independent reasons, any one of which is sufficient:
  1. *Structural.* A map built from RSS is determined only up to rotation; the canonical paper implementing this exact idea states its latent frame has no defined mapping to the world frame, and its own future-work section proposes adding a digital compass. **[SOURCED]** (Ferris et al. §5, §6)
  2. *Signal-to-noise.* At desk scale the true RSS gradient over a 30 cm leg (0.52 dB @ 5 m, n = 2) is below the ESP32's 1 dB quantisation and ~10× below one standard deviation of small-scale fading. **[DERIVED]** + **[SOURCED]**
  3. *Non-averageability.* The dominant error is a spatially-fixed multipath bias, not zero-mean noise, so repeated surveys reproduce it rather than reduce it. **[DERIVED]**
  Best physically achievable desk survey ≈ **±37° per AP**, versus the IMU's ±0.05°/48 s. It cannot bound what it is 700× worse than.
- **(c) Slow drift correction — NO, on this platform.** Drift correction via RSSI position fixes requires baselines of 1.6 m (±30°, Horus-class map) to 49 m (±5°, WiFi-SLAM-class). A robot that "moves in short bursts" in a room does not generate these. And the fixes would be biased, not noisy, so they'd inject a persistent heading offset rather than bound drift. **[DERIVED]**
- **(b) Coarse position / room-level — YES, and it's cheap.** RSSI vectors reliably distinguish rooms and reliably detect "I have been picked up and put somewhere else." Loop-closure-by-RSS-similarity is the one thing the whole literature agrees WiFi is good at. **[SOURCED]** (Ferris et al. §1) Realistic outputs: place classification (dock / desk / floor / other room), kidnapped-robot detection, "am I home" gating. None of these need geometry or heading.

**The honest framing of the user's intuition:** the instinct — *"only an external absolute reference can bound yaw drift indefinitely"* — is exactly right, and is the reason this deserved investigation. The error is in the choice of reference. WiFi APs are stationary in *position*, but 2.4 GHz RSSI is not a smooth, monotone function of position at the centimetre-to-decimetre scale the robot operates at; it is a speckle field with ~5 cm structure and ~5.6 dB variance. The reference is stationary; the *observable* is not usable at this scale.

**Where to look instead (one line each, not researched here):** a magnetometer is the direct fix and is what Ferris et al. themselves propose; optical flow / a downward mouse sensor gives bounded heading over short horizons; a single fixed IR/visual fiducial at the dock gives an absolute yaw anchor for pennies; and the companion agent's rotational/synthetic-aperture line is the one RF approach that could plausibly extract bearing from this hardware.

### 6.2 The minimum experiment that would settle it

**Do Experiment A first. It requires no motion, no mechanical changes, ~30 minutes, and it is decisive.**

**Experiment A — stationary noise floor (30 min, no motion).**
Build a scan-only firmware variant: `WIFI_AP_STA`, `WiFi.scanNetworks()` every 2 s, log `(millis, BSSID, channel, RSSI)` over serial. Park the robot on the desk. Do not touch the room. Run 30 min (~900 scans).
Compute per BSSID: mean, σ, min, max, and *dropout rate* (fraction of scans in which that BSSID was absent).
> **Decision rule — STOP if:** the median per-AP σ over a *stationary* 30 min exceeds **2 dB**, or fewer than 3 APs appear in >95% of scans.
> **Predicted outcome:** σ of 2–5 dB and several APs with >10% dropout. Since the entire signal available from a 30 cm survey leg is ~0.5–1 dB (§5.2), a stationary σ of ≥2 dB ends the investigation on the spot — you would be trying to read a 0.5 dB gradient through 2 dB of stationary jitter *before* adding any spatial fading.

**Experiment B — the gradient test (1 hour, only if A passes).**
Tape a 30 cm square on the desk. Drive to each corner (gyro-closed-loop, which the robot already does well), take 20 scans per corner, repeat the whole square 5 times. For each AP compute the corner-to-corner ΔRSSI per repeat, and the spread of that Δ across repeats.
> **Decision rule — viable only if, for at least 2 APs whose implied gradient directions differ by >45°:** (i) |Δ| > 3 × its across-repeat standard deviation, **and** (ii) the sign of Δ is the same in all 5 repeats.
> **Predicted outcome:** fails both. Δ will be 0–1 LSB and sign-unstable.

**Experiment C — the fallback that probably passes (45 min).**
Pick 5 distinct places: dock, desk centre, desk far corner, floor near desk, adjacent room. 30 scans each. Train a nearest-neighbour classifier on the RSSI vector (use SSD/DIFF features — pairwise RSSI differences — to cancel receiver-side gain, per Hailu et al. §2.2 **[SOURCED]**).
> **Decision rule:** if leave-one-out place-classification accuracy ≥ 90%, ship it — as a **place recogniser and kidnap detector**, explicitly not as a heading or position source.
> **Predicted outcome:** passes easily. This is the useful thing WiFi can actually do for this robot.

**One cheap upgrade if RF is still attractive after A–C:** replace "scan whatever household APs exist" with "one or two dedicated ESP32 beacons at known fixed positions on a known fixed channel with fixed TX power." That removes AP-side TPC/band-steering/mesh-relocation entirely, removes the multi-channel scan cost (single-channel sniffing at beacon rate instead of a 1.3 s+ full scan), and lets you use RSSI *differences* between beacons. It does **not** remove multipath, so it does not change the heading verdict — but it makes every other error source controllable.

---

## Could not verify

- **RADAR's numeric accuracy from the primary source.** Could not retrieve the INFOCOM 2000 PDF: `microsoft.com/.../ieeeinfocom2000.pdf` → 404; `courses.cs.washington.edu/.../radar.pdf` → SSO wall; `cs.cmu.edu/.../radar.pdf` → 403; `dl.acm.org` → 403; CiteSeerX → 404. The published abstract (OpenAlex W2170102584) claims only "a high degree of accuracy." The commonly quoted "median 2–3 m" is secondary and unverified here.
- **Horus's tail accuracy.** Verified median (<0.6 m average) from the ACM abstract; the frequently-quoted "90% within 2.1 m" was not verified.
- **Fink & Kumar, ICRA 2010 (ICRA 2010, DOI 10.1109/robot.2010.5509574)** and **Huang et al., "Efficient, generalized indoor WiFi GraphSLAM," ICRA 2011 (DOI 10.1109/icra.2011.5979643)** — existence, venue, page numbers and citation counts confirmed via OpenAlex; **full texts not retrievable** (`ai.stanford.edu` → 500, `ri.cmu.edu` → 404, both closed-access on IEEE). Their methods are described here only indirectly, via Kolakowski's related-work section.
- **Twigg et al. (ICRA 2012) full text.** Abstract retrieved from Zenodo; the PDF endpoint returned HTTP 422 through the text extractor. Quantitative gradient-estimation baselines, sample counts and experimental scale are therefore **not** established from that paper.
- **ITU-R P.1238 indoor propagation parameters** (authoritative power-loss coefficients N and shadowing σ by environment). Both attempted PDF URLs 404'd; the landing page renders empty. The path-loss numbers used here come from Retscher's Table 4 (peer-reviewed) instead.
- **ESP-IDF default active/passive scan dwell times per channel.** The header documents the struct (`wifi_active_scan_time_t`, `wifi_scan_time_t`, with a warning that values above 1500 ms may cause disconnection) but the *default* values were not located; the ESP-IDF Wi-Fi Driver HTML page is too large to page through efficiently. The ≥1.3 s full-scan figure is derived from the documented 100 ms default beacon interval × 13 channels, and is a **lower bound**, not the measured value.
- **A primary study quantifying fingerprint accuracy decay vs. elapsed time** (e.g. "error grew from X m to Y m after N weeks"). The existence of the 15-month UJI dataset and the framing of the problem are sourced; a specific decay curve is not.
- **Measured azimuthal gain pattern of the ESP32-S3 PCB antenna in this robot's mechanical assembly.** Not attempted; the several-dB-with-nulls statement is inferred from general PCB-antenna behaviour.
- Search engines were largely unusable this session (DuckDuckGo error page, Bing returned unrelated content, Semantic Scholar API HTTP 429, arXiv API HTTP 429/timeout). Discovery was done via OpenAlex, NCBI E-utilities/PMC and direct URL construction.

## Adjacent, not pursued

- **Rotational / synthetic-aperture bearing estimation from a spinning robot** — assigned to the companion agent; noted only where it intersects §4.2's "repeatable place-specific fiducial."
- **WiFi CSI (phase) and 802.11mc FTM/RTT ranging** — CSI fingerprinting reaches 0.09–2.087 m and FTM reaches 0.5–2.10 m (Leitch et al., Table 2), but both require AP-side support and firmware this project does not have.
- **UWB (sub-10 cm) and BLE AoA** — the technologies that actually solve indoor pose; out of scope for "use the WiFi that's already there."
- **Geomagnetic-field fingerprinting** — a magnetometer-based alternative external reference; the direct fix for yaw drift and what Ferris et al. themselves propose adding.
- **Deploying two dedicated ESP32 RF beacons at known positions** instead of scanning household APs — removes AP-side variability and scan latency; sketched in §6.2 but not researched.
- **SSD / HLF / DIFF derived fingerprint features** (Hailu et al. §2.2) — receiver-invariant RSS representations; recommended for Experiment C but not evaluated.
- **Crowdsourced / unsupervised radio-map construction** (MDS on unlabelled signatures, marginalised particle extended GP, GBDT-based AP-change detection) — a rich sub-literature summarised in Kolakowski §2, all building-scale.

## References

### Load-bearing

1. **Ferris, B., Fox, D., Lawrence, N.D. (2007).** "WiFi-SLAM Using Gaussian Process Latent Variable Models." *IJCAI-07*, pp. 2480–2485. https://www.ijcai.org/Proceedings/07/Papers/399.pdf — full text read. Sections used: §1 (three constraints), §2 (why range-only SLAM is infeasible; propagation models are limited), §3.2 (Fig. 1, radial models inadequate), §3.3 + Eq. 2 (rotation-invariant prior and distance-only kernel), §4 Eqs. 14–17 (motion/alignment priors), §5 (3.97 ± 0.59 m; latent-frame ambiguity; right-angle hallucination), §6 (future work: add digital compass).
2. **Kolakowski, M. (2021).** "Automated Calibration of RSS Fingerprinting Based Systems Using a Mobile Robot and Machine Learning." *Sensors* 21(18):6270. DOI 10.3390/s21186270. https://pmc.ncbi.nlm.nih.gov/articles/PMC8473432/ — §1 (survey cost, map staleness), §2 + Table 1 (12-system accuracy comparison, 0.60–4.3 m; WiFi GraphSLAM landmarks 4.3 m), §3–§5 (robot-driven survey needs LiDAR SLAM; 0.1 m binning), Abstract (0.87 m median robot positioning).
3. **Leitch, S.G., Ahmed, Q.Z., Abbas, W.B., Hafeez, M., Lazaridis, P.I., Sureephong, P., Alade, T. (2023).** "On Indoor Localization Using WiFi, BLE, UWB, and IMU Technologies." *Sensors* 23(20):8598. DOI 10.3390/s23208598. https://pmc.ncbi.nlm.nih.gov/articles/PMC10610672/ — §2.2 Table 2 (RSS ranging 2.397 m; RSS fingerprinting 0.169–7.6 m; CSI 0.09–2.087 m; AoA/RTT rows; "RSS values fluctuate when stationary").
4. **Retscher, G. (2020).** "Fundamental Concepts and Evolution of Wi-Fi User Localization: An Overview Based on Different Case Studies." *Sensors* 20(18):5121. DOI 10.3390/s20185121. https://pmc.ncbi.nlm.nih.gov/articles/PMC7570930/ — §3.1.1 Table 4 (γ = 4.2 office / 1.2 corridor / 4.6 @5 GHz), one-slope and multi-wall model limitations, four-orientation survey protocol, trajectory-vs-grid 4× workload reduction.
5. **Hailu, T.G., Guo, X., Si, H., Li, L., Zhang, Y. (2024).** "Theories and Methods for Indoor Positioning Systems: A Comparative Analysis, Challenges, and Prospective Measures." *Sensors* 24(21):6876. DOI 10.3390/s24216876. https://pmc.ncbi.nlm.nih.gov/articles/PMC11548171/ — §2.2 (log-distance + log-normal shadowing formulation; Fig. 2 extreme RSS fluctuation at fixed reference points; SSD/HLF/DIFF derived features; temporal-variation/device-heterogeneity weaknesses; RSS SWOT table).
6. **Twigg, J.N., Fink, J.R., Yu, P.L., Sadler, B.M. (2012).** "RSS gradient-assisted frontier exploration and radio source localization." *ICRA 2012*. DOI 10.1109/icra.2012.6225059. Open record: https://zenodo.org/records/1271335 — abstract only (local RSS sampling → 2-D gradient → frontier prioritisation).
7. **Mendoza-Silva, G.M., Richter, P., Torres-Sospedra, J., Lohan, E.S., Huerta, J. (2018).** "Long-Term WiFi Fingerprinting Dataset for Research on Robust Indoor Positioning." *Data* 3(1):3. DOI 10.3390/data3010003 (CC-BY). — abstract (15-month span; "lack of robustness to short and long time signal changes"; per-position, per-orientation protocol).
8. **Espressif, ESP-IDF v5.3.** `components/esp_wifi/include/esp_wifi_types_generic.h` — `wifi_ap_record_t.rssi` is `int8_t` (1 dBm quantisation); `wifi_scan_time_t` / `wifi_active_scan_time_t` struct definitions. https://raw.githubusercontent.com/espressif/esp-idf/v5.3/components/esp_wifi/include/esp_wifi_types_generic.h
9. **Espressif, ESP-IDF v5.3 Wi-Fi Driver programming guide (ESP32-S3).** https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/api-guides/wifi.html — SoftAP `beacon_interval` default 100 ms (range 100–60000 ms); "the timer used for beacon timeout will be reset during the scanning process."
10. **This repository** — `src/main.cpp:5` (`#include <WiFi.h>`), `:3426` (`kOverrideApChannel = 6`), `:6996` (`WiFi.mode(WIFI_AP)`), `:7004` (`WiFi.softAP(...)`), `:6443-6444` (`resetDriveControl("WiFi transport loss")`). Establishes that the robot is AP-only on a pinned channel and that the WiFi link is the safety-critical control path.
11. **Standard mobile-radio channel theory** (Clarke/Jakes isotropic scattering; Rayleigh fading statistics) — used for the derived figures: λ ≈ 12.3 cm at 2.4 GHz; decorrelation distance 0.3827 λ ≈ 4.7 cm; log-domain σ = (10/ln10)·π/√6 = 5.57 dB; P(>10 dB fade) = 9.5%, P(>20 dB) = 1.0%. Marked **[DERIVED]** throughout; no single retrievable primary source was fetched for these (ITU-R P.1238 and the Puccinelli/Haenggi and Srinivasan/Levis measurement papers were all unreachable — see *Could not verify*).

### Background

12. **Bahl, P., Padmanabhan, V.N. (2000).** "RADAR: an in-building RF-based user location and tracking system." *IEEE INFOCOM 2000*. DOI 10.1109/infcom.2000.832252 (8,366 citations). Abstract only, via OpenAlex W2170102584.
13. **Youssef, M., Agrawala, A. (2005).** "The Horus WLAN location determination system." *MobiSys 2005*. DOI 10.1145/1067170.1067193 (1,654 citations). Abstract only, via OpenAlex W2051376734 — "error less than 0.6 meter on average."
14. **Huang, J., Millman, D., Quigley, M., Stavens, D., Thrun, S., Aggarwal, A. (2011).** "Efficient, generalized indoor WiFi GraphSLAM." *ICRA 2011*, pp. 1038–1043. DOI 10.1109/icra.2011.5979643 (218 citations). Metadata only.
15. **Fink, J., Kumar, V. (2010).** "Online methods for radio signal mapping with mobile robots." *ICRA 2010*. DOI 10.1109/robot.2010.5509574 (108 citations). Metadata only.
16. **Kaemarungsi, K., Krishnamurthy, P. (2004).** "Properties of indoor received signal strength for WLAN location fingerprinting." *MobiQuitous 2004*. DOI 10.1109/mobiq.2004.1331706 (496 citations). Abstract only, via OpenAlex W2118099695 — investigates RSS distributions and Gaussian-model fit for fingerprints.
17. **Wikipedia, "Wi-Fi positioning system."** https://en.wikipedia.org/wiki/Wi-Fi_positioning_system — used only for the secondary claim that RSSI trilateration gives 2–4 m median and fingerprinting 0.6 m median / 1.3 m tail, and for the AoA/MUSIC framing (ArrayTrack, SpotFi, LTEye). Secondary source; treat accordingly.

---

### Note to orchestrator

Two items worth surfacing to the user directly, because they are the load-bearing facts and they are short enough to quote:

1. Ferris/Fox/Lawrence built *exactly this system* and wrote, in the results section, that the latent map has no defined relationship to the world coordinate frame — and their proposed fix in the conclusion is "add a digital compass." The most authoritative instance of the idea concedes the heading point explicitly.
2. On this hardware the arithmetic is brutally simple: a 30 cm survey leg 5 m from an AP produces ~0.5 dB of true signal, and the ESP32 reports RSSI in whole dBm. The measurement cannot resolve the quantity even before multipath is considered.

The 30-minute stationary scan-logging test (Experiment A) needs no motion code and no mechanical work, and its predicted result (2–5 dB of jitter while completely still) settles the question against a 0.5 dB signal.