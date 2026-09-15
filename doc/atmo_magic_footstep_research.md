# Atmo Magic footsteps: literature review of physically based contact-sound synthesis and material perception

Research pass for the PBR-derived footstep/impact acoustic class feature (see
`V:/Scratch/atmo/review/pbr_acoustic_taxonomy_reviewed.md` for the design under review). Sources are papers,
official engine documentation and engine source; blog summaries were only used to locate primaries.

## Summary: what the literature says we got right, and wrong

The design's three load-bearing choices survive contact with the literature. Merging tile/marble/concrete/stone/glass
into one HARD class is not a compromise, it is what listeners actually do: identification of impacted materials is
near-perfect *between* gross categories and collapses *within* them. Keeping GRANULAR as a separate body rather than a
variant is the single best-supported distinction in the whole footstep literature — solid/aggregate confusion in a
walked-upon-material study is 1.5%. Treating roughness as a continuous modulator that never selects a family matches
both the only shipped engine that has an equivalent scalar (Source's `roughnessFactor`, which only picks scrape
variants) and the only synthesis model that parameterises roughness at all (van den Doel's fractal-dimension noise).
Three things need changing. First, attack time is a property of the *shoe*, not the floor, and the design currently
varies it per class; that will read as the avatar changing footwear, not the ground changing. Second, the detail-layer
gain should come from roughness, not from classifier residual confidence — the residual is an epistemic quantity and
the literature ties the noise layer to a physical one. Third, the ±2-semitone pitch cap is far more conservative than
it needs to be: material percept is stable until frequency moves by about an octave. Also, the reviewers' slogan
"listeners are poor at identifying material but excellent at detecting change" is only half right and the half that is
wrong matters: listeners are *excellent* at absolute gross-category identification, and poor only at fine
within-category discrimination.

## 1. Modal synthesis of contact sounds

- The canonical model is a bank of damped sinusoids: a modal model `M = {f, d, A}` with a vector of modal frequencies
  `f`, decay rates `d`, and a gain matrix `A` whose entry `a_nk` is mode *n*'s gain at contact location *k*.
  Frequencies and dampings come from geometry plus material; gains come from mode shapes and therefore from *where*
  you hit. (van den Doel, Kry & Pai, 2001, *FoleyAutomatic: physically-based sound effects for interactive simulation
  and animation*, http://www.cs.ubc.ca/~kvdoel/publications/foleyautomatic.pdf)
- Under Rayleigh damping `C = αM + βK`, mode *i* has damping `d_i = ½(α + βλ_i)` and frequency
  `f_i = (1/2π)·√(λ_i − ((α + βλ_i)/2)²)`, where `λ_i` are eigenvalues of `KU = λMU`. `M` and `K` depend on density ρ,
  Young's modulus E and Poisson's ratio ν; holding ν fixed, `λ_i` scales linearly with `γ = E/ρ`. So the whole material
  is four numbers: α, β, γ and an impulse scale. (Ren, Yeh & Lin, 2013, *Example-Guided Physically Based Modal Sound
  Synthesis*, ACM TOG 32(1), http://gamma-web.iacs.umd.edu/AUDIO_MATERIAL/examplebasedsoundsynthesis.pdf)
- Their fitted parameters make the material/size split explicit. β (the stiffness-proportional, high-frequency damping
  term) spans about 150× across materials — metal 2.116e-8, porcelain 8.414e-8, glass 1.434e-7, plastic 8.775e-7, wood
  3.083e-6 — while γ = E/ρ spans only about 7.5× (plastic 8.90e4 to wood 6.66e5). Damping is where the material lives;
  γ and geometry set pitch, i.e. size. (Ren, Yeh & Lin, 2013, Table I, same URL)
- The authors state that α and β transfer to another object "with no drastic shape or size change", and demonstrate
  transferring one porcelain plate's parameters to a smaller plate and to a bunny. This is the formal justification for
  a per-material class that is independent of the object it is applied to. (Ren, Yeh & Lin, 2013, §3 and Fig. 12, same
  URL)
- Linear modal synthesis alone does not match a recording; a stochastic residual must be added and transferred
  alongside the modes. (Ren, Yeh & Lin, 2013, §6, same URL)
- Full FEM simulation of surface vibration produces correct sound but is far too slow for interactive use, which is why
  everything real-time is modal. (O'Brien, Cook & Essl, 2001, *Synthesizing Sounds from Physically Based Motion*,
  SIGGRAPH 2001, https://bpb-us-w2.wpmucdn.com/sites.uwm.edu/dist/0/236/files/2016/09/siggraph01-1j7zofk.pdf)
- Radiation from complex geometry can be precomputed per mode as an acoustic transfer function, decoupling "which modes
  ring" from "how loud they are at the listener". (James, Barbič & Pai, 2006, *Precomputed acoustic transfer:
  output-sensitive, accurate sound generation for geometrically complex vibration sources*, ACM TOG 25(3) 987–995,
  https://dl.acm.org/doi/10.1145/1141911.1141983 — abstract only)
- Treating objects as rigid during contact resolution loses micro-collisions, chattering and contact-dependent damping;
  modelling them is what makes contact sound "high quality". (Zheng & James, 2011, *Toward High-Quality Modal Contact
  Sound*, ACM TOG 30(4), https://www.cs.cornell.edu/projects/Sound/mc/ — project page and abstract only)

## 2. Material perception from impact sound

- A shape-invariant decay parameter dominates perceived material; differences in decay and in frequency both affect
  similarity judgements but decay plays a substantially larger role, and it is decay *rate*, not total energy or
  duration, that matters. (Klatzky, Pai & Krotkov, 2000, *Perception of Material from Contact Sounds*, Presence 9(4)
  399–410, https://doi.org/10.1162/105474600566907 — abstract via publisher listing; the stimulus design and results
  are described in detail by Giordano & McAdams 2006 §I, which I read in full)
- Listener performance is *perfect* for gross categories (steel–glass vs wood–plexiglass) and impaired within them,
  where responses are driven by object size instead. 88% of individual listeners were perfectly correct on the gross
  split regardless of plate geometry. Damping explained the gross split — but so did signal duration and spectral
  centre of gravity, so the study concludes there is "overall poor support for the relevance of damping" as *the*
  cue. Within a gross category, listeners went on signal frequency. (Giordano & McAdams, 2006, *Material
  identification of real impact sounds: effects of size variation in steel, glass, wood, and plexiglass plates*, JASA
  119(2) 1171–1181, https://brungio.github.io/BLG_SMC_2006_JASA.pdf)
- Frequency only shifts the *material* percept when it varies over at least an octave; smaller variation reads as size.
  Roussarie found a variation equivalent to a perfect fifth had no effect on material identification at all.
  (Giordano & McAdams, 2006, §I review of Klatzky et al. 2000, Avanzini & Rocchesso 2001, Roussarie 1999, same URL)
- Measured confusion, real recordings, 53 listeners, five response categories: real wood was called wood 50.7% and
  plastic 47.9%; real porcelain was called *glass* 83.7% and porcelain only 15.1%; real metal was called metal 66.1%
  and glass 24.2%; real glass 73.3%. Wood/plastic and glass/porcelain are effectively single classes.
  (Ren, Yeh & Lin, 2013, Table III, http://gamma-web.iacs.umd.edu/AUDIO_MATERIAL/examplebasedsoundsynthesis.pdf)
- Synthetic stimuli can degrade performance further by letting listeners over-weight frequency. (Lutfi & Oh 1997, as
  reported in Giordano & McAdams, 2006, §I, same URL. Lutfi, 2008, *Human Sound Source Identification*, in *Auditory
  Perception of Sound Sources*, Springer, pp. 13–42, https://link.springer.com/chapter/10.1007/978-0-387-71305-2_2 —
  UNVERIFIED, abstract not fetched)
- Acoustical variables account for only about half the variance in everyday-sound identification accuracy; ecological
  frequency (how often you meet that sound) and causal uncertainty account for the rest. (Ballas 1993, as reported in
  Turchet, 2016, §2.1, https://www.lucaturchet.it/PUBLIC_DOWNLOADS/publications/journals/Footstep_sounds_synthesis.pdf)

## 3. Footstep-specific synthesis: the solid/aggregate dichotomy

- Ground materials fall into four typologies: solid (hard, homogeneous — asphalt, wood), aggregate (granular — gravel,
  snow), liquid (puddles), and hybrid (mud, wet gravel). Solids are rendered with an impact model, aggregates with up
  to three instances of PhISEM plus a crumpling model, wet variants by multiplying the dry signal with a liquid model.
  All four typologies were "very well discriminated" in listening tests. (Turchet, 2016, *Footstep sounds synthesis*,
  Applied Acoustics 107:46–68, Tables 1–2 and §5,
  https://www.lucaturchet.it/PUBLIC_DOWNLOADS/publications/journals/Footstep_sounds_synthesis.pdf)
- Walked-upon materials: solids were "rarely confused with aggregates and vice versa", grand-average confusion 0.015
  (range 0.00–0.03) across auditory, haptic, kinesthetic and audio-haptic conditions. Within a category, confusions
  tracked hardness (solids) or grain size (aggregates). Auditory-only identification was the *worst* modality tested.
  (Giordano, Visell, Yao, Hayward, Cooperstock & McAdams, 2012, *Identification of walked-upon materials in auditory,
  kinesthetic, haptic, and audio-haptic conditions*, JASA 131(5) 4002–4012,
  https://s3.amazonaws.com/rtlab/Giordano2012Identification.pdf)
- PhISEM's granular parameter set is small and directly usable: particle count `N`, system-energy decay pole `s`,
  per-particle noise decay pole `c`, and a second-order resonator pole radius `r` and centre frequency `f`. Cook's own
  sweep used N ∈ {2,4,8,16,64,256,1024}, s ∈ {0.95…0.9999}, c = 0.95 (≈130 samples to −60 dB), r ∈ {0.7…0.99},
  f ∈ {1000…8000} Hz. (Cook, 2002, *Modeling Bill's Gait: Analysis and Parametric Synthesis of Walking Sounds*, AES
  22nd Int. Conf., §3 and §5, https://soundlab.cs.princeton.edu/publications/billsgait.pdf)
- Grain size maps to resonator frequency (bigger grains → lower), and aggregate compliance maps to particle count and
  sound duration. (Turchet, 2016, §4.5, same URL)
- Sole hardness, not floor material, controls attack. Mallet/hammer hardness is predictable from the attack alone
  (Freed), and hammer hardness and object hardness are confused with each other in identification tasks. Turchet
  models shoe type by shaping the exciter's attack and peak while explicitly keeping the material percept unchanged.
  (Turchet, 2016, §3.3 and §4.5, same URL)
- Footstep synthesis is driven by a ground-reaction-force curve, not by a single impulse; Farnell uses a three-segment
  polynomial GRF for heel, roll and ball. (Farnell, 2007, *Marching onwards: procedural synthetic footsteps for video
  games and animation*, Audio Mostly 2007,
  https://www.semanticscholar.org/paper/4364dbb2433c98da69b138245a26af4dd2aa5f5d — UNVERIFIED, located via
  Semantic Scholar listing, full text not fetched; the GRF description is corroborated by Turchet, 2016, §1)
- Interactivity and coherent context both raise recognition. Interactive synthesis matched real recordings; passive
  listening to the same synthesised sounds was significantly worse. A coherent ambient soundscape significantly
  improved surface-material recognition versus footsteps alone or an incoherent soundscape. (Nordahl, Serafin &
  Turchet 2010 and Turchet, Serafin & Nordahl 2010, as reported in Turchet, 2016, §§2.1 and 5, same URL)

## 4. Roughness, friction and micro-geometry

- van den Doel's sliding model excites the modal bank with fractal noise whose power spectrum is ∝ f^−α; α is the
  roughness parameter and relates to fractal dimension by `D = α/2 + 2`. The reported D values (1.17–1.39) come from
  *profilometry of machined surfaces at a 10⁻⁶ m length scale*, and from fitting a contact-microphone recording — the
  surface structure was measured at 0.03 mm resolution. Scrape amplitude goes as `v·F_normal`. The model "only works
  well when surface roughness is homogeneous over a large area". (van den Doel, Kry & Pai, 2001, §4.2,
  http://www.cs.ubc.ca/~kvdoel/publications/foleyautomatic.pdf)
- The three-level surface representation is macro (mesh), *meso (bump/normal/displacement maps)*, micro (fractal
  friction noise). The meso level is the only published route from a rendering texture to sound: normals sampled along
  the contact path are converted to a time-varying normal-direction impulse, so the audible grain correlates with the
  visible bumps. (Ren, Yeh & Lin, 2010, *Synthesizing Contact Sounds Between Textured Models*, IEEE VR 2010, §5,
  http://gamma.cs.unc.edu/SlidingSound/VR2010.pdf)
- Nothing in either paper links a *BRDF* roughness scalar to sound. The published roughness parameters are a measured
  height-profile fractal dimension and a normal map — not a microfacet lobe width.

## 5. Game engine practice

- Source engine ships the most detailed acoustic material table in a commercial engine, and it is entirely hand-authored.
  `surfaceaudioparams_t` holds `reflectivity`, `hardnessFactor` ("like elasticity, but only affects impact sound
  choices"), `roughnessFactor` ("like friction, but only affects scrape sound choices"), plus `roughThreshold`,
  `hardThreshold` and `hardVelocityThreshold`; sounds are named in `surfacesoundnames_t` as `stepleft`, `stepright`,
  `impactSoft`, `impactHard`, `scrapeSmooth`, `scrapeRough`, `bulletImpact`, `rolling`, `breakSound`, `strainSound`.
  Note "Hard impacts must meet both hardnessFactor AND velocity thresholds". (Valve, *vphysics_interface.h*, Source SDK
  2013, https://raw.githubusercontent.com/ValveSoftware/source-sdk-2013/master/src/public/vphysics_interface.h)
- A shipped `surfaceproperties.txt` defines ~106 surface classes with `base` inheritance, mixing physics
  (density/elasticity/friction/thickness/dampening) and the audio keys above. The class is a string tag on the material
  (`$surfaceprop`), never derived from a rendering parameter. (Facepunch, *garrysmod/scripts/surfaceproperties.txt*,
  https://raw.githubusercontent.com/Facepunch/garrysmod/master/garrysmod/scripts/surfaceproperties.txt)
- Unreal exposes 62 nameable Surface Types configured in Project Settings → Physics → Physical Surface, carried on a
  Physical Material asset that is attached to a rendering Material. Physical Material Masks add a per-texel variant via
  an 8-colour mask texture. Nothing is derived from base colour, metallic or roughness. (Epic Games, *Add a Surface
  Type in Unreal Engine*, https://dev.epicgames.com/documentation/en-us/unreal-engine/add-a-surface-type-in-unreal-engine;
  *Physical Materials in Unreal Engine*,
  https://dev.epicgames.com/documentation/en-us/unreal-engine/physical-materials-in-unreal-engine)
- Unity's `PhysicsMaterial` has exactly five fields — `bounciness`, `dynamicFriction`, `staticFriction`,
  `bounceCombine`, `frictionCombine` — and no audio field at all. Unity footsteps are tag- or
  terrain-layer-driven by convention, not by an engine facility. (Unity, *PhysicsMaterial* scripting reference,
  https://docs.unity3d.com/6000.0/Documentation/ScriptReference/PhysicsMaterial.html)
- id Tech 3 shipped the smallest set: `surfaceparm metalsteps`, `surfaceparm flesh`, `surfaceparm nosteps`, or nothing
  (default). The manual states it is "not possible for a designer to create or assign a specific sound routine to a
  texture" beyond those. (id Software, *Quake III Arena Shader Manual*, §Q3MAP surface parameter directives,
  https://q3map2.robotrenegade.com/docs/shader_manual/q3map-surface-parameter-directives.html)
- Middleware ships small class counts by default. Audiokinetic's own footstep tutorial demonstrates four:
  `Walk_Concrete`, `Walk_Grass`, `Walk_Tiles`, `Walk_Wood`, under a Switch Container (not a State, because Switches are
  game-object-scoped and States are global), with Concrete as the fallback for unrecognised material names.
  (Audiokinetic, *Footsteps Material Management using Wwise / Unreal Engine 4 / Unity 3D*,
  https://www.audiokinetic.com/en/blog/footsteps-material-management-using-wwise-unreal-engine-4-unity-3d/ —
  UNVERIFIED body text, 403 on fetch; details taken from the mirrored copy at
  https://github.com/akchina/learnwwisecn and the search-indexed excerpt)
- FMOD's equivalent is a labeled discrete parameter selecting a multi-instrument, again typically four labels.
  (UNVERIFIED — no official FMOD documentation page fetched; only third-party tutorials located.)

## 6. Inferring material for sound from visual data

- Owens et al. trained on 977 videos of a drumstick hitting things (46,577 actions; classes include ceramic, metal,
  plastic, glass, wood, cloth, grass, leaves, rock, gravel, carpet, tile, drywall, plastic bag) and predicted
  cochleagram subband envelopes. Material classification from the *real sound* reached 45.8% class-averaged accuracy
  (chance 5.9%); from *vision* it reached 42.0% with whole-image ImageNet features and 52.9% with cropped patches of
  the impact region; from the model's *predicted* sound, 22.7%. Vision and sound classifiers made different errors.
  (Owens, Isola, McDermott, Torralba, Adelson & Freeman, 2016, *Visually Indicated Sounds*, CVPR 2016,
  https://ar5iv.labs.arxiv.org/html/1512.08512)
- ObjectFolder encodes each object as implicit networks (VisionNet / AudioNet / TouchNet) — appearance and impact sound
  are stored side by side per object, but the acoustic side is derived from FEM modal analysis of the mesh with
  assigned material constants, not inferred from appearance. (Gao et al., 2022, *ObjectFolder 2.0*,
  https://arxiv.org/pdf/2204.02389; Gao et al., 2021, *ObjectFolder*, https://arxiv.org/html/2109.07991)
- Impact sound from video needs physics priors bolted on because "existing video-driven deep learning approaches lack
  physics knowledge and only capture weak correspondence between visual content and impact sounds". (Su, Qian,
  Shlizerman, Torralba & Gan, 2023, *Physics-Driven Diffusion Models for Impact Sound Synthesis from Videos*, CVPR
  2023, https://arxiv.org/abs/2303.16897 — abstract and project description)
- The one deployed visual→acoustic pipeline is for room absorption, not contact sound, and it uses a CNN semantic
  segmentation of appearance on the premise that "the visual appearance and the acoustic materials have a strong
  association". (Kim, Kwon & Yoon, 2019, *Real-time 3-D Mapping with Estimating Acoustic Materials*,
  https://arxiv.org/abs/1909.06998)

## Implications for the Soapstorm design

Ranked by how much they change the current spec.

1. **Move attack time out of the class table and onto the avatar.** The design assigns per-class attack (<5 ms HARD,
   5 ms WOOD, 10 ms SOFT). Attack is the hammer's signature: mallet hardness is predictable from attack alone, and
   hammer/object hardness are perceptually confused, so varying attack per surface will be heard as the avatar
   changing shoes. Hold attack fixed per avatar/shoe and let classes differ in decay and spectral centroid only.
   [Turchet 2016 §3.3, §4.5]
2. **Drive the detail layer from roughness, not from residual classifier confidence.** The residual/noise layer is
   physically motivated (modal synthesis alone does not match a recording) and roughness is the published control for
   scrape/grain noise; classifier residual is an epistemic number with no acoustic meaning, and using it makes the
   grit level fluctuate with our uncertainty rather than with the floor. [Ren, Yeh & Lin 2013 §6; van den Doel et al.
   2001 §4.2]
3. **Prefer the normal/bump map over the MR roughness scalar as the grit driver where one exists.** The only published
   texture→sound path is meso-level: sample normals along the contact path and convert deviation into impulses. PBR
   roughness has no published relationship to contact sound at all — the "roughness" in the synthesis literature is a
   height-profile fractal dimension measured at 10⁻⁶ m. Keep MR roughness as an uncalibrated fallback modulating grit
   gain and LPF cutoff only, exactly as the design already specifies, and record it as an unvalidated heuristic.
   [Ren, Yeh & Lin 2010 §5; van den Doel et al. 2001 §4.2]
4. **Raise the pitch cap from ±2 to about ±4 semitones.** Frequency shifts the material percept only when it ranges
   over roughly an octave; a perfect fifth produced no material effect. ±4 semitones is a third of an octave, safely
   inside the size-only regime, and gives noticeably more per-object variation. [Giordano & McAdams 2006 §I]
5. **Make UNKNOWN a mid-damping neutral, not HARD-ish.** Wood/plastic is the perceptual centroid of the impact space
   (wood called wood 50.7% / plastic 47.9%), so an error there is the least audible error available. A bright HARD
   default commits to the long-decay, high-centroid corner that listeners *do* discriminate reliably, so every miss is
   maximally wrong. Target roughly 80–150 ms decay, 0.8–2 kHz — i.e. the current WOOD_SOLID body. [Ren, Yeh & Lin
   2013 Table III; Klatzky et al. 2000]
6. **Keep the ship-first five, and keep GRANULAR first among them.** Solid vs aggregate is the most reliable
   distinction in the whole literature (1.5% cross-category confusion) and it maps onto SL's dominant surface, terrain.
   HARD merging glass/stone/tile is correct; METAL stays separate not because it is a different gross category
   (it is not — steel and glass are one) but because its ring duration is long enough to be the one within-category
   split listeners handle. [Giordano et al. 2012; Giordano & McAdams 2006]
7. **Adopt Source's two-variant-per-axis structure rather than more classes.** Source ships 106 named surfaces but only
   three audio scalars plus thresholds, selecting `impactHard`/`impactSoft` and `scrapeRough`/`scrapeSmooth`. Add
   Source's `hardVelocityThreshold` idea: hard impacts require *both* a hard surface and a fast contact. Our step
   velocity is already available from the avatar. [Valve, vphysics_interface.h]
8. **Set GRANULAR's parameters from PhISEM directly.** Particle count N and system decay pole s from terrain layer
   (dirt → high N, low s; gravel → low N, high s), resonator centre frequency from grain size (1–8 kHz), per-particle
   decay pole ≈0.95. These are Cook's own sweep values and give a defensible starting table with no tuning by ear.
   [Cook 2002 §3, §5]
9. **Restate the reviewers' slogan accurately in the design doc.** Listeners are near-perfect at *absolute*
   gross-category identification (88% of listeners perfect on steel-glass vs wood-plexiglass, independent of
   geometry) and poor only within a category, where they fall back on size. This strengthens rather than weakens the
   case for a small merged class list, and it means a wrong *category* is genuinely audible — which is the real
   argument for hysteresis, not "nobody can tell anyway". [Giordano & McAdams 2006]
10. **Lean on the existing soundscape when confidence is low.** Coherent ambient context significantly improves
    surface-material recognition versus footsteps in isolation, and interactive (self-generated) footsteps are
    recognised far better than passively heard ones — both conditions we already have. A cheap correct-sounding class
    plus SSSoundscape ambience beats an expensive uncertain one. [Turchet 2016 §§2.1, 5]
11. **Hysteresis has no direct primary support — mark it as engineering judgement.** The nearest evidence is that
    acoustical variables explain only about half the variance in everyday-sound identification, with ecological
    frequency and causal uncertainty explaining the rest; that supports priors and stickiness over per-step accuracy,
    but no paper measured switching thresholds. The 2–3 step / >2 m rule stays a judgement call. [Ballas 1993 via
    Turchet 2016 §2.1] — UNVERIFIED as a quantitative claim.
12. **The cavity/hollow axis is defensible, but frame it as size, not material.** Frequency is the size cue; a
    low-frequency cavity mode will be heard as "big hollow thing", which is what we want, and it is correctly gated on
    geometry rather than on PBR in the current design. No change needed, but the doc should say *why*. [Giordano &
    McAdams 2006 §I]
13. **Expected accuracy ceiling: about 50%.** Vision-based material classification from cropped impact-region patches
    reached 52.9% on 17 classes, against 45.8% from the real sound. Our PBR signal is weaker than an image patch, so
    the design's expected step mix (UNKNOWN 45–60%) is realistic rather than pessimistic, and no engine ships a
    render-derived acoustic class — every one of Source, Unreal, Unity, Quake 3, Wwise and FMOD hand-tags it. If this
    ships it is, as far as this review found, novel. [Owens et al. 2016; Valve; Epic; Unity; id Software]

## Sources

- van den Doel, Kry & Pai (2001). FoleyAutomatic: physically-based sound effects for interactive simulation and animation. SIGGRAPH 2001. http://www.cs.ubc.ca/~kvdoel/publications/foleyautomatic.pdf
- O'Brien, Cook & Essl (2001). Synthesizing Sounds from Physically Based Motion. SIGGRAPH 2001. https://bpb-us-w2.wpmucdn.com/sites.uwm.edu/dist/0/236/files/2016/09/siggraph01-1j7zofk.pdf
- James, Barbič & Pai (2006). Precomputed acoustic transfer. ACM TOG 25(3) 987–995. https://dl.acm.org/doi/10.1145/1141911.1141983 (abstract only)
- Zheng & James (2011). Toward High-Quality Modal Contact Sound. ACM TOG 30(4). https://www.cs.cornell.edu/projects/Sound/mc/ (project page only)
- Ren, Yeh & Lin (2013). Example-Guided Physically Based Modal Sound Synthesis. ACM TOG 32(1). http://gamma-web.iacs.umd.edu/AUDIO_MATERIAL/examplebasedsoundsynthesis.pdf
- Ren, Yeh & Lin (2010). Synthesizing Contact Sounds Between Textured Models. IEEE VR 2010. http://gamma.cs.unc.edu/SlidingSound/VR2010.pdf
- Klatzky, Pai & Krotkov (2000). Perception of Material from Contact Sounds. Presence 9(4) 399–410. https://doi.org/10.1162/105474600566907 (abstract/secondary description only)
- Giordano & McAdams (2006). Material identification of real impact sounds. JASA 119(2) 1171–1181. https://brungio.github.io/BLG_SMC_2006_JASA.pdf
- Giordano, Visell, Yao, Hayward, Cooperstock & McAdams (2012). Identification of walked-upon materials. JASA 131(5) 4002–4012. https://s3.amazonaws.com/rtlab/Giordano2012Identification.pdf
- Lutfi (2008). Human Sound Source Identification. Springer Handbook of Auditory Research 29, 13–42. https://link.springer.com/chapter/10.1007/978-0-387-71305-2_2 (UNVERIFIED)
- Cook (2002). Modeling Bill's Gait. AES 22nd Int. Conf. https://soundlab.cs.princeton.edu/publications/billsgait.pdf
- Turchet (2016). Footstep sounds synthesis. Applied Acoustics 107:46–68. https://www.lucaturchet.it/PUBLIC_DOWNLOADS/publications/journals/Footstep_sounds_synthesis.pdf
- Farnell (2007). Marching onwards. Audio Mostly 2007. https://www.semanticscholar.org/paper/4364dbb2433c98da69b138245a26af4dd2aa5f5d (UNVERIFIED)
- Valve. vphysics_interface.h, Source SDK 2013. https://raw.githubusercontent.com/ValveSoftware/source-sdk-2013/master/src/public/vphysics_interface.h
- Facepunch. garrysmod/scripts/surfaceproperties.txt. https://raw.githubusercontent.com/Facepunch/garrysmod/master/garrysmod/scripts/surfaceproperties.txt
- Epic Games. Physical Materials / Add a Surface Type. https://dev.epicgames.com/documentation/en-us/unreal-engine/add-a-surface-type-in-unreal-engine
- Unity. PhysicsMaterial. https://docs.unity3d.com/6000.0/Documentation/ScriptReference/PhysicsMaterial.html
- id Software. Quake III Arena Shader Manual. https://q3map2.robotrenegade.com/docs/shader_manual/q3map-surface-parameter-directives.html
- Audiokinetic. Footsteps Material Management using Wwise. https://www.audiokinetic.com/en/blog/footsteps-material-management-using-wwise-unreal-engine-4-unity-3d/ (UNVERIFIED body)
- Owens, Isola, McDermott, Torralba, Adelson & Freeman (2016). Visually Indicated Sounds. CVPR 2016. https://ar5iv.labs.arxiv.org/html/1512.08512
- Gao et al. (2021/2022). ObjectFolder / ObjectFolder 2.0. https://arxiv.org/html/2109.07991, https://arxiv.org/pdf/2204.02389
- Su, Qian, Shlizerman, Torralba & Gan (2023). Physics-Driven Diffusion Models for Impact Sound Synthesis from Videos. CVPR 2023. https://arxiv.org/abs/2303.16897
- Kim, Kwon & Yoon (2019). Real-time 3-D Mapping with Estimating Acoustic Materials. https://arxiv.org/abs/1909.06998
