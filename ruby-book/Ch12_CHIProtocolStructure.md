# Chapter 12: CHI Protocol Structure in gem5

> *CHI is large enough that the only sane way to learn it is through the code organization itself.*

This chapter will decompose gem5's CHI implementation into its constituent SLICC files: messages (CHI-msg.sm), cache behavior (CHI-cache*.sm), memory behavior (CHI-mem.sm), and DVM support (CHI-dvm-misc-node*.sm).
It will map CHI concepts (request/response/snoop/data channels, RN-F, HN-F, SN-F node roles) to concrete source files and message flows.
The CHIGenericController will be explained as the bridge between SLICC-generated protocol logic and the Ruby controller framework.
The failure-mode section will show how readers drown in CHI terminology unless message channels and node roles are tied immediately to source files.
By the end, readers can navigate the CHI protocol tree and explain where each type of transaction behavior lives in the code.
