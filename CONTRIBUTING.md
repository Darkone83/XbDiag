# Contributing to XbDiag

Thank you for your interest in contributing to XbDiag.

XbDiag is an open-source diagnostic and utility tool for the original Xbox. The goal of this project is to provide useful, accurate, and reliable tools for the Xbox homebrew and modding community.

Contributions, fixes, testing, documentation improvements, and feedback are welcome.

---

## License

XbDiag is licensed under the MIT License.

By submitting code, documentation, assets, or other contributions to this repository, you agree that your contribution will be licensed under the same MIT License as the rest of the project unless explicitly stated otherwise.

This means your contribution may be used, modified, redistributed, and included in future versions of XbDiag under the MIT License.

---

## Attribution

If you use, modify, copy, redistribute, or include substantial portions of XbDiag in another project, you must preserve the copyright notice and MIT License text as required by the MIT License.

Preferred attribution:

> Portions of this software are based on XbDiag by Darkone83.  
> Original project: https://github.com/Darkone83/XbDiag  
> Licensed under the MIT License.

Credit is important. This project is provided freely for the community, but proper attribution helps preserve the work and history behind it.

---

## How to Contribute

You can contribute by:

- Reporting bugs
- Testing on real hardware
- Improving documentation
- Suggesting features
- Submitting code fixes
- Improving compatibility across Xbox revisions
- Helping verify behavior on different BIOS, dashboard, and hardware configurations

---

## Pull Requests

When submitting a pull request:

1. Keep the change focused.
2. Clearly describe what the change does.
3. Mention what hardware, Xbox revision, BIOS, or dashboard you tested with if applicable.
4. Avoid unrelated formatting changes.
5. Preserve existing code comments and annotations unless they are incorrect.
6. Do not remove attribution, license headers, or existing credit notices.
7. Keep changes compatible with the existing project structure unless there is a clear reason to refactor.

Small, focused pull requests are easier to review and more likely to be accepted.

---

## Code Style

Please try to follow the existing style of the project.

General expectations:

- Keep code readable and maintainable.
- Avoid unnecessary rewrites.
- Prefer direct fixes over large unrelated refactors.
- Comment hardware-specific behavior where useful.
- Be careful with assumptions around Xbox revisions, sensors, EEPROM behavior, SMBus behavior, BIOS behavior, and display handling.
- Preserve compatibility unless the change intentionally targets a specific platform or revision.

---

## Testing

Because XbDiag targets original Xbox hardware, testing details are very helpful.

When possible, include:

- Xbox revision tested
- BIOS or modchip used
- Dashboard or launch environment
- Storage setup if relevant
- Whether the issue occurs on real hardware, emulator, or both
- Screenshots or photos when useful
- Steps to reproduce the issue

For hardware-specific changes, real hardware testing is preferred whenever possible.

---

## Bug Reports

When reporting a bug, please include:

- XbDiag version or commit used
- Xbox revision
- BIOS or modchip
- Dashboard or launch method
- What you expected to happen
- What actually happened
- Steps to reproduce the issue
- Any screenshots, logs, or photos that may help

The more detail provided, the easier it is to reproduce and fix the issue.

---

## Feature Requests

Feature requests are welcome.

When suggesting a feature, please explain:

- What problem it solves
- Why it would be useful
- Whether it applies to all Xbox revisions or specific hardware
- Any known references, examples, or existing tools that behave similarly

Not every feature will be accepted, but thoughtful suggestions are appreciated.

---

## Community Expectations

Please keep discussion respectful and constructive.

This project exists to help the original Xbox community. Feedback, disagreement, and technical debate are welcome, but personal attacks, harassment, or intentionally disruptive behavior are not.

---

## Credit and Project History

XbDiag is maintained by Darkone83.

Community feedback, testing, and contributions help shape the project. Contributors may be credited in release notes, documentation, or the repository when appropriate.

If your work is based on XbDiag, please preserve proper credit and license notices.
