/** These are the states for the "low-level" physical bit detector state machine */
enum nexa_bit_detector_state {
    WaitBitStart,
    WaitBitHiLo,
    WaitBitLoDecision,
};

/** These are the different conditions that can be signaled by the "low level" physical bit detector */
enum nexa_condition {
    SpaceConditionDetected,
    MarkConditionDetected,
    SyncConditionDetected,
    PauseConditionDetected,
    PhysicalBitErrorBadLowTime,
    PhysicalBitErrorBadHighTime,
    PhysicalBitErrorBadEdge1,
    PhysicalBitErrorBadEdge2,
    PhysicalBitErrorBadEdge3
};

enum nexa_telegram_detector_state {
    WaitSyncCondition,
    WaitLogicalBitStart,
    WaitSpaceCondition,
    ProtocolError,
    WaitMarkCondition,
};