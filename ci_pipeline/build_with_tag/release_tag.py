"""Git tag validation and filenames shared by the release and sync tools."""

import hashlib
import re
import subprocess


HASH_PREFIX = "tag-sha256-"
PACKAGE_TAG = r"[A-Za-z0-9][A-Za-z0-9._-]{0,100}"


def valid_tag(tag):
    return bool(tag and subprocess.run(
        ["git", "check-ref-format", f"refs/tags/{tag}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0)


def tag_slug(tag):
    if re.fullmatch(PACKAGE_TAG, tag) and not tag.startswith(HASH_PREFIX):
        return tag
    return HASH_PREFIX + hashlib.sha256(tag.encode("utf-8")).hexdigest()


def release_recorded_commit(release):
    """Read the commit recorded by the API or the Release's build section."""
    target = release.get("target_commitish")
    target = target if isinstance(target, str) and re.fullmatch(r"[0-9a-f]{40}", target) else None

    # The generator appends this section after the changelog. Match a standalone
    # heading and stop at the next heading when reading a Commit field.
    sections = re.split(r"^ {0,3}(#{1,6})[ \t]+([^\n]+)$",
                        (release.get("body") or "").replace("\r\n", "\n"), flags=re.M)
    from_body = None
    headings = list(zip(sections[1::3], sections[2::3], sections[3::3]))
    for level, title, fields in reversed(headings):
        if level == "##" and title.strip() == "构建信息":
            commits = re.findall(r"^[ \t]*- Commit:[ \t]*`([0-9a-f]{40})`[ \t]*$", fields, re.M)
            if len(set(commits)) > 1:
                raise ValueError("Release build information records conflicting commits")
            from_body = commits[0] if commits else None
            break
    if target and from_body and target != from_body:
        raise ValueError(f"Release records conflicting commits: target_commitish {target} vs body {from_body}")
    return target or from_body
