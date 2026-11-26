# Repository policy

This file holds policy only. Facts, versions, absolute paths and procedure belong in the source that owns them, in a config, or in a generated document.

- One fact has one home. Cite or generate it; never retype it.
- Change control runs in order: plan, spec, decision record, code. Superseded decisions stay visible and are marked, never silently removed.
- A worker changes only files inside its own seam.
- A claim without a command is unverified. A check that never failed proves nothing. Report the command and its exit beside every result.
- Documentation checks start in report mode. Strict mode is earned by a clean report, per repository.
- Keep existing project documentation, manual and support links intact. When a duplicated fact appears here, move it to the document or source that owns it and link to it.
- Archives are frozen and are not documentation migration targets.
- The vendored documentation gate pins the checker version and hash in its config. Do not edit the checker to make a finding disappear.
