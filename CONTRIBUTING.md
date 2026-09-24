# Contributing

## Commit messages

[Conventional Commits](https://www.conventionalcommits.org/). A `commit-msg`
hook rejects anything else, so install the hooks once:

```sh
pre-commit install
```

Types that reach the CHANGELOG:

| Type       | Section    | Version bump |
| ---------- | ---------- | ------------ |
| `feat`     | Features   | minor        |
| `fix`      | Bug Fixes  | patch        |
| `refactor` | Refactors  | patch        |

`docs`, `test`, `build`, `ci`, `chore` and `style` are accepted but never
appear in the CHANGELOG and never bump the version. `perf` is accepted by the
tooling but unused here: a change that makes the same API faster or smaller is
a `refactor`, and anything a user can newly *do* is a `feat`.

The bump matters more than the heading. This component is below 1.0, so a
dependency on `^0.1.0` picks up a patch release but not a minor one - `feat`
means existing users have to update their dependency, `fix` and `refactor`
reach them automatically.

Scopes are free-form and optional: `feat(controllers):`, `fix(panels):`.

### Breaking changes

Mark the subject with `!` **and** add a footer. The `!` drives the version
bump; the footer is what puts it in its own CHANGELOG section:

```
feat(api)!: rename the panel descriptor's gate_lines field

BREAKING CHANGE: out-of-tree panel descriptors must be updated.
```

Below 1.0 a breaking change bumps the minor version, not the major one.

## Releasing

```sh
cz bump                  # bumps idf_component.yml + CHANGELOG, commits, tags
git push --follow-tags
```

Pushing the tag builds the GitHub Release from the CHANGELOG section and
publishes to the component registry over OIDC. Edit the CHANGELOG after
`cz bump` and before pushing if a release needs context the commits do not
carry.

## Code style

`clang-format` runs as a pre-commit hook; `.clang-format` is the source of
truth. Note that it sorts and regroups includes.
