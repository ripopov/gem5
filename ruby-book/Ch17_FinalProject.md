# Chapter 17: Final Project: Extend a Protocol or Network Model

> *Mastery means changing the model responsibly, not just reading it.*

This chapter will guide readers through implementing and validating a meaningful extension to gem5's memory or NoC stack.
It will offer two paths: a protocol extension (adding states/messages to MI_example, validated with RubyTester) and a NoC extension (custom routing or topology, validated with GarnetSyntheticTraffic).
The chapter will cover the full workflow: design, SLICC/C++ implementation, build integration, verification, and performance measurement.
The failure-mode section will address the danger of adding states, messages, or routing behavior without a verification and measurement story.
By the end, readers finish the book by implementing, testing, and evaluating one meaningful extension — proving they can modify gem5, not just read it.
