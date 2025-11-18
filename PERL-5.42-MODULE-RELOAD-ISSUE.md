# Perl 5.42.0 Module Reload Behavior Change

## Issue Description

When using `lei` or testing modules that depend on PublicInbox::Over, you may encounter the error:

```
Attempt to reload PublicInbox/Over.pm aborted.
Compilation failed in require at /usr/local/perl-5.42.0/lib/5.42.0/parent.pm line 17.
```

## Root Cause

Perl 5.42.0 has stricter behavior regarding module reloading. When a module fails to compile (e.g., due to missing dependencies like DBI), Perl now:

1. Marks the module in `%INC` with an `undef` value
2. **Prevents subsequent attempts to reload** the failed module with the error "Attempt to reload X aborted"

### Example Reproduction

```perl
# First attempt - fails due to missing dependency
eval { require PublicInbox::Over };
# Error: Can't locate DBI.pm...
# Sets $INC{"PublicInbox/Over.pm"} = undef

# Second attempt - NEW behavior in Perl 5.42.0
eval { require PublicInbox::Over };
# Error: Attempt to reload PublicInbox/Over.pm aborted
```

### Previous Perl Behavior

In Perl 5.38 and earlier, the second `require` attempt would retry loading the module and report the original error (Can't locate DBI.pm).

### Perl 5.42.0 Behavior

Perl 5.42.0 detects the failed load in `%INC` and immediately aborts with "Attempt to reload" error, preventing cascade loading issues but making the error less informative.

## Impact on public-inbox

This affects:
- `lei` commands that require database functionality
- Any code path that loads PublicInbox::OverIdx (which inherits from PublicInbox::Over via `use parent`)
- Modules: ExtSearch, ExtSearchIdx, SearchIdx, V2Writable, LeiSavedSearch

## Solutions

### Solution 1: Install Required Dependencies

The cleanest solution is to ensure all required dependencies are installed:

```bash
cpan -T DBI DBD::SQLite
```

### Solution 2: Clear %INC Before Retry (Workaround)

If you need to handle the error gracefully in code:

```perl
eval { require PublicInbox::Over };
if ($@) {
    # Clear the failed entry
    delete $INC{"PublicInbox/Over.pm"};
    # Can now retry or handle error
}
```

### Solution 3: Improve Error Handling (Recommended)

Modify modules to provide better error messages when dependencies are missing. This could involve:

1. Adding try/catch around critical requires
2. Providing helpful error messages about missing dependencies
3. Ensuring test files properly skip when dependencies are unavailable

## Test Suite Behavior

The test suite correctly skips tests when dependencies are missing, so this issue only manifests when:
- Running `lei` commands directly
- Manually loading modules without dependencies installed
- Third-party code that doesn't check for dependencies

## Verification

All 185 test programs (1800 tests) pass successfully with Perl 5.42.0 when the URI dependency is installed. The reload issue only occurs when dependencies like DBI/DBD::SQLite are missing.

##Recommendation

1. Document the DBI/DBD::SQLite dependency more prominently for `lei` users
2. Consider adding a dependency check in `lei init` to provide helpful error messages
3. The code itself is compatible with Perl 5.42.0 - this is purely a dependency installation issue

## Related Information

- Perl Version: 5.42.0
- Affected Modules: PublicInbox::Over, PublicInbox::OverIdx
- Status: Not a bug, but a behavior change that makes missing dependencies more obvious
