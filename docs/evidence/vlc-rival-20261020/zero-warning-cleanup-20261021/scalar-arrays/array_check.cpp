#define NOMINMAX
#include <windows.h>
#include <oleauto.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
namespace Legacy {
SAFEARRAY* CreateIntArray(const int* values, size_t count) {
    SAFEARRAYBOUND bound{};
    bound.cElements = static_cast<ULONG>(count);
    bound.lLbound = 0;
    SAFEARRAY* sa = SafeArrayCreate(VT_I4, 1, &bound);
    if (!sa) return nullptr;
    for (LONG i = 0; i < static_cast<LONG>(count); ++i) { SafeArrayPutElement(sa, &i, const_cast<int*>(&values[i])); }
    return sa;
}

SAFEARRAY* CreateDoubleArray(const double* values, size_t count) {
    SAFEARRAYBOUND bound{};
    bound.cElements = static_cast<ULONG>(count);
    bound.lLbound = 0;
    SAFEARRAY* sa = SafeArrayCreate(VT_R8, 1, &bound);
    if (!sa) return nullptr;
    for (LONG i = 0; i < static_cast<LONG>(count); ++i) { SafeArrayPutElement(sa, &i, const_cast<double*>(&values[i])); }
    return sa;
}

}
namespace Candidate {
template<VARTYPE Type, typename T>
SAFEARRAY* CreateScalarArray(const T* values, size_t count) {
    if (count > LONG_MAX || (!values && count)) return nullptr;
    SAFEARRAYBOUND bound{static_cast<ULONG>(count), 0};
    SAFEARRAY* array = SafeArrayCreate(Type, 1, &bound);
    if (!array || !count) return array;
    void* data = nullptr;
    if (FAILED(SafeArrayAccessData(array, &data))) { SafeArrayDestroy(array); return nullptr; }
    std::copy_n(values, count, static_cast<T*>(data));
    if (FAILED(SafeArrayUnaccessData(array))) { SafeArrayDestroy(array); return nullptr; }
    return array;
}

SAFEARRAY* CreateIntArray(const int* values, size_t count) { return CreateScalarArray<VT_I4>(values, count); }
SAFEARRAY* CreateDoubleArray(const double* values, size_t count) { return CreateScalarArray<VT_R8>(values, count); }

}
using Clock=std::chrono::steady_clock;
template<typename T> using Factory=SAFEARRAY*(*)(const T*,size_t);
template<typename T> void Verify(Factory<T> factory,const std::vector<T>& values,VARTYPE expected) {
 SAFEARRAY* array=factory(values.data(),values.size());if(!array)throw 1;
 VARTYPE type=VT_EMPTY;LONG low=0,high=0;
 if(FAILED(SafeArrayGetVartype(array,&type))||type!=expected||SafeArrayGetDim(array)!=1||FAILED(SafeArrayGetLBound(array,1,&low))||FAILED(SafeArrayGetUBound(array,1,&high))||low!=0||high!=static_cast<LONG>(values.size())-1)throw 2;
 if(!values.empty()){void* data=nullptr;if(FAILED(SafeArrayAccessData(array,&data))||!data||std::memcmp(data,values.data(),values.size()*sizeof(T))!=0)throw 3;if(FAILED(SafeArrayUnaccessData(array)))throw 4;}
 if(FAILED(SafeArrayDestroy(array)))throw 5;
}
template<typename T> double Time(Factory<T> factory,const std::vector<T>& values,int loops){
 auto start=Clock::now();for(int n=0;n<loops;++n){auto array=factory(values.data(),values.size());if(!array||FAILED(SafeArrayDestroy(array)))throw 6;}
 return std::chrono::duration<double,std::nano>(Clock::now()-start).count()/loops;
}
struct Stats{double median,min,max;};
Stats Summarize(std::vector<double> values){std::sort(values.begin(),values.end());return{values[values.size()/2],values.front(),values.back()};}
bool first=true;
template<typename T> void Run(const char* label,VARTYPE type,Factory<T> legacy,Factory<T> candidate){
 for(size_t count:{size_t(0),size_t(2),size_t(4),size_t(1000),size_t(10000)}){
  std::vector<T> values(count);for(size_t i=0;i<count;++i)values[i]=static_cast<T>(static_cast<int>(i%271)-135)/static_cast<T>(4);
  if(type==VT_R8&&count>1){values[0]=static_cast<T>(-0.0);values[1]=std::numeric_limits<T>::quiet_NaN();}
  Verify(legacy,values,type);Verify(candidate,values,type);
  auto original=legacy(values.data(),values.size()),changed=candidate(values.data(),values.size());
  if(!original||!changed||original->fFeatures!=changed->fFeatures)throw 7;
  SAFEARRAYBOUND grown{static_cast<ULONG>(count+1),0};
  if(SafeArrayRedim(original,&grown)!=S_OK||SafeArrayRedim(changed,&grown)!=S_OK)throw 8;
  if(FAILED(SafeArrayDestroy(original))||FAILED(SafeArrayDestroy(changed)))throw 9;
  Time(legacy,values,10);Time(candidate,values,10);
  int loops=count>=10000?80:count>=1000?500:10000;std::vector<double> a,b;
  for(int round=0;round<9;++round){if(round%2){b.push_back(Time(candidate,values,loops));a.push_back(Time(legacy,values,loops));}else{a.push_back(Time(legacy,values,loops));b.push_back(Time(candidate,values,loops));}}
  auto sa=Summarize(a),sb=Summarize(b);if(!first)std::cout<<",";first=false;
  std::cout<<"{\"type\":\""<<label<<"\",\"elements\":"<<count<<",\"loopsPerBlock\":"<<loops<<",\"blocks\":9,\"legacyNs\":{\"median\":"<<sa.median<<",\"min\":"<<sa.min<<",\"max\":"<<sa.max<<"},\"candidateNs\":{\"median\":"<<sb.median<<",\"min\":"<<sb.min<<",\"max\":"<<sb.max<<"},\"legacyOverCandidate\":"<<sa.median/sb.median<<"}";
 }
}
int main(){try{std::cout<<"{\"scope\":\"Exact production scalar SAFEARRAY helpers; original array flags and resize behavior preserved; no UI or whole-app ranking\",\"verifiedCases\":10,\"rows\":[";Run<int>("I4",VT_I4,Legacy::CreateIntArray,Candidate::CreateIntArray);Run<double>("R8",VT_R8,Legacy::CreateDoubleArray,Candidate::CreateDoubleArray);std::cout<<"]}";}catch(int error){std::cerr<<"Array check failed:"<<error;return error;}return 0;}
