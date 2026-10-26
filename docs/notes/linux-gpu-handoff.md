# Handoff: free Linux GPUs for GPU Blueprint

Brief for an agent working next to the main sopt session (owner, 2026-10-26). Read `CLAUDE.md` first: its rules
apply here too. The ones that matter most for this task:
- Tokens are limited: keep turns lean.
- No personal data in the repository (names, accounts, e-mail addresses).
- Ask the owner before any design change outside this brief.
- Commits end with the session's attribution trailer.

## Goal

GPU Blueprint (OpBench, TexBench, StageBench, ShaderInfo in `tools/windows`) measures how fast each shader operation
runs on a GPU. The reports feed sopt's cost models (`docs/opbench`, `src/ir/ops.cpp`). Today only Windows PCs with
D3D11 can run it, so the data comes from the owner's own cards and from testers.

The plan:
1. Find a free (or almost free) cloud service with real GPUs that we can drive from scripts.
2. Port the measurements to Linux / Vulkan (OpBench's kernels are compute shaders, so this is feasible). The port is
   a separate task for the main session.
3. Run the measurements there, ideally started from GitHub Actions, so they cost no tokens.

**Your task is step 1:** pick the service and prepare the owner's account setup. You may not create accounts, sign in
or enter credentials yourself: the owner does that, following steps you write down.

## Candidates to check

Check each one's current terms. Things change, and these notes are from a quick search.

| Service | GPU (as reported) | Notes to verify |
|---|---|---|
| Kaggle Notebooks | T4 x2, P100; about 30 GPU-hours a week | Has an API: `kaggle kernels push` runs a notebook or script and its output can be fetched. With the API token as a GitHub secret this could run from Actions. Probably the best fit. |
| Google Colab (free) | T4 | Interactive only; hard to automate. |
| Lightning AI | free monthly credits; SSH and terminal | Check what the credits buy and whether a CLI can start jobs. |
| Amazon SageMaker Studio Lab | T4, a few hours a day | Check whether it still exists and how sessions start. |
| Paperspace Gradient (free tier) | varies | Check whether a free GPU tier still exists. |
| Hugging Face ZeroGPU | H200 slices | Bound to Gradio apps; likely unsuitable. |
| Intel Tiber AI Cloud | Gaudi / Data Center GPU Max | "Free to start" per listings; Max is Xe-HPC, not a consumer family. |
| AMD Developer Cloud | MI300X | Free hours seen only in regional programs; CDNA 3 is not a gaming family. |

## What makes a service useful

- **GPU families:** sopt models consumer families. A data-center chip helps only when it shares its architecture
  with a consumer one:
  - T4 = Turing (TU104), like the GTX 16 / RTX 20 models.
  - L4 = Ada (AD104), like `nvidia-ada`.
  - A10 / A10G = Ampere (GA102), like `nvidia-ampere`.
  - P100 (GP100), A100 (GA100), H100 / H200 (Hopper) and MI300X (CDNA 3) differ from the consumer chips, for example in
    fp16 rate. They are still useful as Vulkan driver test beds, but not as data for the models.
- **Vulkan:** check that `vulkaninfo --summary` (package `vulkan-tools`) shows the GPU, not only a software device
  (llvmpipe). Many notebook containers have the CUDA driver but no Vulkan ICD (`/usr/share/vulkan/icd.d/nvidia_icd.json`)
  or `libGLX_nvidia`. Find out whether it can be installed without root.
- **Automation:** can a job be started and its output fetched by a script (API / CLI with a token)? Can that run from
  a GitHub Actions job?
- **Exclusive GPU:** a shared or partitioned GPU (MIG, time-slicing) disturbs timing. OpBench measures ratios between
  operations, so steady clocks matter more than peak speed.
- **Limits:** session length, weekly quota, idle timeouts, region (the owner is in Denmark), and what is needed for
  signup (phone verification, credit card). A credit card required for a "free" tier is worth a warning.
- **Terms of service:** benchmarking and automated runs must be allowed.

## Deliverables

1. `docs/notes/linux-gpu-services.md`: a comparison table of the services you checked, one recommendation with
   reasons, and the owner's account setup steps for it (what to click, which token to create, which repo secret name
   to use). Mark every unverified claim as such.
2. If the recommended service can run scripts: a minimal test job under `tools/linux/` (for Kaggle, a kernel script
   and its `kernel-metadata.json`). It should print `nvidia-smi` and `vulkaninfo --summary` and run one tiny Vulkan
   compute dispatch if that is easy. Don't run it yourself: the owner runs it with the steps from deliverable 1, or
   wires it into a workflow after setting the secret.
3. A short summary for the owner in chat: the pick, and what he has to do.

Ask the owner if something is unclear. Don't start the Linux / Vulkan port of OpBench: that is the main session's
next step once a service works.
