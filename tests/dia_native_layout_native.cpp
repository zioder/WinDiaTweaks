#include "../src/dia_native_layout.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <algorithm>

template<class T> T Read(const std::vector<uint8_t>& bytes,size_t at) {
    T value{}; if (at+sizeof(T)>bytes.size()) return value;
    memcpy(&value,bytes.data()+at,sizeof(T)); return value;
}
template<class T> void Write(std::vector<uint8_t>& bytes,size_t at,T value) { memcpy(bytes.data()+at,&value,sizeof(T)); }
size_t Offset(const std::vector<uint8_t>& bytes,uint32_t rva) {
    uint32_t nt=Read<uint32_t>(bytes,60); uint16_t count=Read<uint16_t>(bytes,nt+6),opt=Read<uint16_t>(bytes,nt+20);
    for (unsigned i=0;i<count;++i) {
        size_t s=nt+24+opt+i*40; uint32_t start=Read<uint32_t>(bytes,s+12),size=Read<uint32_t>(bytes,s+16);
        if (rva>=start && uint64_t(rva)<uint64_t(start)+size) return Read<uint32_t>(bytes,s+20)+rva-start;
    }
    return bytes.size();
}
bool Mutations(const std::vector<uint8_t>& original,const dia_native::NativeLayout& layout) {
    auto reject=[](const auto& bytes,const char* name) {
        auto result=dia_native::Resolve(bytes); printf("  %s: %s\n",name,result.supported?"FAIL":"PASS"); return !result.supported;
    };
    bool ok=true; auto bytes=original;
    bytes.resize(bytes.size()/2); ok=reject(bytes,"truncated image")&&ok;
    bytes=original; bytes[Offset(bytes,layout.windowBranch)]=0x90;
    ok=reject(bytes,"changed local window control flow")&&ok;
    uint32_t termination=0,engine=0;
    for(const auto& field:layout.applicationFields) {
        if(field.name=="terminationState") termination=field.codeOffset;
        if(field.name=="hasInitializedBrowserEngine") engine=field.codeOffset;
    }
    for(const auto& patch:layout.patches) if(patch.capability=="local window identity guard" ||
        patch.capability=="queued local window identity guard") {
        for(uint32_t field:{termination,engine}) {
            std::vector<uint32_t> branches;
            for(uint32_t rva=patch.rva-256;rva+14<patch.rva;++rva) {
                size_t at=Offset(original,rva);
                if(at+14>=original.size()) continue;
                if(original[at]==0x41 && original[at+1]==0x80 && original[at+2]==0xBD &&
                    Read<uint32_t>(original,at+3)==field && original[at+8]==0x0F && original[at+9]==0x85) branches.push_back(rva+8);
            }
            if(branches.size()!=1) {printf("  readiness mutation site discovery: FAIL\n");ok=false;continue;}
            bytes=original; bytes[Offset(bytes,branches.front())+1]=0x84;
            ok=reject(bytes,field==termination?"reversed termination rejection":"reversed engine rejection")&&ok;
            if(field==engine) {
                bytes=original;
                Write<int32_t>(bytes,Offset(bytes,branches.front())+2,int32_t(patch.rva-branches.front()-6));
                ok=reject(bytes,"engine rejection redirected to window creation")&&ok;
            }
        }
    }
    auto missing=std::find_if(layout.patches.begin(),layout.patches.end(),[](const auto& p){return p.capability=="local window availability missing identity";});
    auto empty=std::find_if(layout.patches.begin(),layout.patches.end(),[](const auto& p){return p.capability=="local window availability empty identity";});
    std::vector<uint32_t> cleanupCalls;
    if(missing!=layout.patches.end() && empty!=layout.patches.end()) {
        for(uint32_t rva=missing->rva+6;rva+10<empty->rva;++rva) {
            size_t at=Offset(original,rva);
            if(original[at]==0x48 && original[at+1]==0x8D && original[at+2]==0x4C && original[at+3]==0x24 &&
                (original[at+4]==0x20 || original[at+4]==0x40) && original[at+5]==0xE8) cleanupCalls.push_back(rva+5);
        }
    }
    if(cleanupCalls.size()!=2) {printf("  cleanup mutation site discovery: FAIL\n");ok=false;}
    else {
        bytes=original; size_t at=Offset(bytes,cleanupCalls[1]); std::fill(bytes.begin()+at,bytes.begin()+at+5,uint8_t(0x90));
        ok=reject(bytes,"removed owned-result cleanup")&&ok;
        bytes=original; uint32_t firstTarget=uint32_t(int64_t(cleanupCalls[0])+5+Read<int32_t>(original,Offset(original,cleanupCalls[0])+1));
        Write<int32_t>(bytes,at+1,int32_t(firstTarget-cleanupCalls[1]-5));
        ok=reject(bytes,"cleanup redirected to different valid destroy type")&&ok;
    }
    bytes=original;
    auto allow=std::find_if(layout.patches.begin(),layout.patches.end(),[](const auto& p){return !p.executable;});
    if (allow==layout.patches.end()) return false;
    bytes[Offset(bytes,allow->rva)+31]=0x7F;
    ok=reject(bytes,"occupied command array padding")&&ok;
    bytes=original;
    uint32_t nt=Read<uint32_t>(bytes,60); Write<uint32_t>(bytes,nt+8,0x12345678);
    bool timestamp=dia_native::Resolve(bytes).supported;
    printf("  changed timestamp admitted: %s\n",timestamp?"PASS":"FAIL"); ok=timestamp&&ok;
    // Duplicate a complete nominal descriptor in file-backed zero padding.
    // Its relative references are rebased, so a resolver selecting the first
    // matching name instead of rejecting ambiguity would wrongly admit it.
    bytes=original; uint16_t count=Read<uint16_t>(bytes,nt+6),opt=Read<uint16_t>(bytes,nt+20);
    bool duplicated=false;
    for (unsigned i=0;i<count&&!duplicated;++i) {
        size_t s=nt+24+opt+i*40;
        if (Read<uint32_t>(bytes,s+36)&0x20000000) continue;
        uint32_t start=Read<uint32_t>(bytes,s+12),size=Read<uint32_t>(bytes,s+16),raw=Read<uint32_t>(bytes,s+20);
        for (uint32_t n=0;n+64<size;n+=4) {
            if (!std::all_of(bytes.begin()+raw+n,bytes.begin()+raw+n+64,[](uint8_t b){return b==0;})) continue;
            uint32_t target=start+n; size_t source=Offset(bytes,layout.applicationDescriptor);
            memcpy(bytes.data()+raw+n,bytes.data()+source,44);
            for (uint32_t f:{4u,8u,12u,16u,20u}) {
                int32_t relative=Read<int32_t>(original,source+f);
                if (relative) Write<int32_t>(bytes,raw+n+f,int32_t(int64_t(layout.applicationDescriptor)+f+relative-target-f));
            }
            duplicated=true; break;
        }
    }
    ok=duplicated&&reject(bytes,"ambiguous Swift controller descriptor")&&ok;
    return ok;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        bool ok=true;
        for (size_t size:{0u,1u,63u,64u,128u,512u}) {
            std::vector<uint8_t> bytes(size,0xFF);
            if(size>=64) {Write<uint16_t>(bytes,0,0x5A4D);Write<uint32_t>(bytes,60,0xFFFFFFF0);}
            ok=ok&&!dia_native::Resolve(bytes).supported;
        }
        printf("malformed PE bounds: %s\n",ok?"PASS":"FAIL"); return ok?0:1;
    }
    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        auto result = dia_native::ResolveFile(argv[i]);
        printf("supported=%u descriptor=%x cache=%x: %s\n", result.supported,
            result.layout.applicationDescriptor, result.layout.metadataCache, result.message.c_str());
        for (const auto& patch : result.layout.patches)
            printf("  %s rva=%x bytes=%zu\n", patch.capability.c_str(), patch.rva, patch.original.size());
        printf("  bootstrap=%x frameSelf=%x frameCondition=%x restored=%u launchTag=%x/%x terminationNil=%u\n",
            result.layout.bootstrapBranch,result.layout.bootstrapFrameSelf,result.layout.bootstrapFrameCondition,
            result.layout.restoredAuthenticationOrdinal,result.layout.launchTagOffset,result.layout.launchCompleteSuccessTag,result.layout.terminationNilTag);
        for (const auto& f:result.layout.applicationFields) printf("  %s index=%u offset=%x\n",f.name.c_str(),f.index,f.codeOffset);
        if(result.supported) {
            std::ifstream file(std::filesystem::path(argv[i]),std::ios::binary);
            std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
            ok=Mutations(bytes,result.layout)&&ok;
        }
        ok = ok && result.supported;
    }
    return ok ? 0 : 1;
}
