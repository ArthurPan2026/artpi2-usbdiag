# USB enumeration session rules and test matrix

This matrix describes the current shared C99 session implementation. Tests are
written against fixed vectors and expected outcomes; the implementation does
not generate its own expected verdicts. A session passes only after a successful
enumeration end, complete evidence, and no applicable `FAIL` or
`NOT_OBSERVED` result.

| Rule | Passing evidence | Violating evidence | Insufficient evidence | Current test coverage |
|---|---|---|---|---|
| `DEV_B_LENGTH` | Complete device descriptor has `bLength=18` | Wrong declared length | Descriptor/field absent or truncated | Complete-session, single-rule failure, truncation/gap tests |
| `DEV_TYPE` | Device descriptor type is 1 | Wrong descriptor type | Type byte absent | Complete-session, single-rule failure, missing-field boundary tests |
| `DEV_8B_PREFIX` | First 8 bytes equal the later full device response | Repeated prefix mismatch or full response differs | No paired response; first event forgotten | Prefix-pair, sticky-first-failure, forget-event tests |
| `DEV_FIELDS` | EP0 packet size is legal for the observed speed | Speed-specific value is illegal | Speed unknown or field absent | Complete-session, single-rule failure, generated boundaries |
| `CFG_B_LENGTH` | Configuration header length is 9 and type is 2 | Wrong length or type | Header truncated before required bytes | Complete-session, single-rule failure, truncated input tests |
| `CFG_TOTAL_LENGTH` | Complete configuration transfer agrees with `wTotalLength` | Impossible declaration or known full-transfer mismatch | Only header/partial transfer; transfer length unknown | Complete-session, single-rule failure, header-only and boundary tests |
| `CFG_TREE_WALK` | Every descriptor is bounded and has a legal length | Invalid descriptor length, malformed type, or out-of-range walk | Capture is incomplete/truncated | Complete-session, malformed-tree failure, gap/truncation tests |
| `CFG_INTERFACE_COUNT` | Unique interface numbers match `bNumInterfaces` | Declared and observed counts differ | No complete configuration | Complete-session, single-rule failure, incomplete configuration tests |
| `IF_ENDPOINT_COUNT` | Each interface/alternate setting has its declared endpoints | Per-alternate count mismatch or orphan endpoint | Structure is not fully captured | Complete-session, single-rule failure, alternate/count tests |
| `EP_PACKET_SIZE` | Endpoint size is legal for speed and transfer type | Illegal size/reserved transaction bits | Required speed/descriptor fields unknown | Complete-session, single-rule failure, fifth-failure/reference-cap test |
| `EP_INTERVAL` | Periodic endpoint interval is legal for speed/type | Zero or speed/type-invalid interval | Speed or interval field absent | Complete-session, single-rule failure, generated boundaries |
| `EP0_STALL` | Required Device/Configuration descriptor, `SET_ADDRESS`, and `SET_CONFIGURATION` requests do not STALL | A required request STALLs | Required request unseen or has a non-STALL transfer error | Complete-session, required-request STALL, optional-string STALL tests |

## Session-level invariants

- `test_complete_twelve_rule_session` checks a complete enumeration and every
  rule's public definition and source reference.
- `test_each_individual_rule_failure` constructs an independent violation for
  every rule that has a failing condition.
- `test_missing_and_dropped_evidence_never_passes` covers forgotten prefix
  evidence, truncation, and an explicit queue-loss gap.
- `test_prefix_witness_is_sticky` verifies later valid data cannot overwrite a
  recorded failure witness.
- `test_ep0_rules_and_optional_stall` distinguishes required-request STALL,
  missing required requests, and an optional string-request STALL.
- `test_configuration_counts_and_applicability` covers no-endpoint
  `NOT_APPLICABLE` and a header-only configuration.
- `test_fifth_failure_and_wrapped_clock` verifies four retained references,
  omitted-failure accounting, sticky verdicts, and unsigned timer wrap.
- `test_context_isolation_and_input_errors` checks interleaved sessions and
  invalid API arguments.
- `test_fixed_seed_session_boundaries` runs 10,000 deterministic generated
  inputs and asserts session invariants with assertions enabled even in a
  Release-configured test build.

The host suite is not a substitute for captured board evidence. It validates
the rule engine's semantics; it does not claim hardware fault injection or a
real USB device failure.
