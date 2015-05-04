// Bindings
#include "PyROOT.h"
#include "Cppyy.h"
#include "TCallContext.h"

// ROOT
#include "TBaseClass.h"
#include "TClass.h"
#include "TClassRef.h"
#include "TClassTable.h"
#include "TClassEdit.h"
#include "TCollection.h"
#include "TDataMember.h"
#include "TDataType.h"
#include "TError.h"
#include "TFunction.h"
#include "TGlobal.h"
#include "TInterpreter.h"
#include "TInterpreterValue.h"
#include "TList.h"
#include "TMethod.h"
#include "TMethodArg.h"
#include "TROOT.h"

// Standard
#include <assert.h>
#include <map>
#include <sstream>
#include <vector>

// temp
#include <iostream>
typedef PyROOT::TParameter TParameter;
// --temp


// data for life time management ---------------------------------------------
typedef std::vector< TClassRef > ClassRefs_t;
static ClassRefs_t g_classrefs( 1 );
static const ClassRefs_t::size_type GLOBAL_HANDLE = 1;

typedef std::map< std::string, ClassRefs_t::size_type > Name2ClassRefIndex_t;
static Name2ClassRefIndex_t g_name2classrefidx;

typedef std::map< Cppyy::TCppMethod_t, CallFunc_t* > Method2CallFunc_t;
static Method2CallFunc_t g_method2callfunc;

typedef std::vector< TFunction > GlobalFuncs_t;
static GlobalFuncs_t g_globalfuncs;

typedef std::vector< TGlobal* > GlobalVars_t;
static GlobalVars_t g_globalvars;

// data ----------------------------------------------------------------------
Cppyy::TCppScope_t Cppyy::gGlobalScope = GLOBAL_HANDLE;


// global initialization -----------------------------------------------------
namespace {

class ApplicationStarter {
public:
   ApplicationStarter() {
      // setup dummy holders for global and std namespaces
      assert( g_classrefs.size() == GLOBAL_HANDLE );
      g_name2classrefidx[ "" ]      = GLOBAL_HANDLE;
      g_classrefs.push_back(TClassRef(""));
      // ROOT ignores std/::std, so point them to the global namespace
      g_name2classrefidx[ "std" ]   = GLOBAL_HANDLE;
      g_name2classrefidx[ "::std" ] = GLOBAL_HANDLE;
      // add a dummy global to refer to as null at index 0
      g_globalvars.push_back( nullptr );
   }

   ~ApplicationStarter() {
      for ( auto ifunc : g_method2callfunc )
         gInterpreter->CallFunc_Delete( ifunc.second );
   }
} _applicationStarter;

} // unnamed namespace


// local helpers -------------------------------------------------------------
static inline
TClassRef& type_from_handle( Cppyy::TCppScope_t scope )
{
   assert( (ClassRefs_t::size_type) scope < g_classrefs.size());
   return g_classrefs[ (ClassRefs_t::size_type)scope ];
}

// type_from_handle to go here
static inline
TFunction* type_get_method( Cppyy::TCppType_t klass, Cppyy::TCppIndex_t idx )
{
   TClassRef& cr = type_from_handle( klass );
   if ( cr.GetClass() )
      return (TFunction*)cr->GetListOfMethods()->At( idx );
   assert( klass == (Cppyy::TCppType_t)GLOBAL_HANDLE );
   return (TFunction*)idx;
}

static inline
Cppyy::TCppScope_t declaring_scope( Cppyy::TCppMethod_t method )
{
   TMethod* m = dynamic_cast<TMethod*>( (TFunction*)method );
   if ( m ) return Cppyy::GetScope( m->GetClass()->GetName() );
   return (Cppyy::TCppScope_t)GLOBAL_HANDLE;
}


// name to opaque C++ scope representation -----------------------------------
Cppyy::TCppIndex_t Cppyy::GetNumScopes( TCppScope_t scope )
{
   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() ) return 0;   // not supported if not at global scope
   assert( scope == (TCppScope_t)GLOBAL_HANDLE );
   return gClassTable->Classes();
}

std::string Cppyy::GetScopeName( TCppScope_t parent, TCppIndex_t iscope ) 
{
// Retrieve the scope name of the scope indexed with iscope in parent.
   TClassRef& cr = type_from_handle( parent );
   if ( cr.GetClass() ) return 0;   // not supported if not at global scope
   assert( parent == (TCppScope_t)GLOBAL_HANDLE );
   std::string name = gClassTable->At( iscope );
   if ( name.find("::") == std::string::npos )
       return name;
   return "";
}

std::string Cppyy::ResolveName( const std::string& cppitem_name )
{
// Fully resolve the given name to the final type name.
   std::string tclean = TClassEdit::CleanType( cppitem_name.c_str() );

   TDataType* dt = gROOT->GetType( tclean.c_str() );
   if ( dt ) return dt->GetFullTypeName();
   return TClassEdit::ResolveTypedef( tclean.c_str(), true );
}

Cppyy::TCppScope_t Cppyy::GetScope( const std::string& sname )
{
   std::string scope_name;
   if ( sname.find( "std::", 0, 5 ) == 0 )
      scope_name = sname.substr( 5, std::string::npos );
   else
      scope_name = sname;

   auto icr = g_name2classrefidx.find( scope_name );
   if ( icr != g_name2classrefidx.end() )
      return (TCppType_t)icr->second;

   // use TClass directly, to enable auto-loading
   TClassRef cr( TClass::GetClass( scope_name.c_str(), kTRUE, kTRUE ) );
   if ( !cr.GetClass() )
      return (TCppScope_t)NULL;

   // no check for ClassInfo as forward declared classes are okay (fragile)

   ClassRefs_t::size_type sz = g_classrefs.size();
   g_name2classrefidx[ scope_name ] = sz;
   g_classrefs.push_back( TClassRef( scope_name.c_str() ) );
   return (TCppScope_t)sz;
}

Cppyy::TCppType_t Cppyy::GetTemplate( const std::string& /* template_name */ )
{
   return (TCppType_t)0;
}

Cppyy::TCppType_t Cppyy::GetActualClass( TCppType_t klass, TCppObject_t obj )
{
   TClassRef& cr = type_from_handle( klass );
   TClass* clActual = cr->GetActualClass( (void*)obj );
   if ( clActual && clActual != cr.GetClass() ) {
      // TODO: lookup through name should not be needed
      return (TCppType_t)GetScope( clActual->GetName() );
   }
   return klass;
}

size_t Cppyy::SizeOf( TCppType_t klass )
{
   TClassRef& cr = type_from_handle( klass );
   if ( cr.GetClass() ) return (size_t)cr->Size();
   return (size_t)0;
}

Bool_t Cppyy::IsBuiltin( const std::string& type_name )
{
    TDataType* dt = gROOT->GetType( TClassEdit::CleanType( type_name.c_str(), 1 ).c_str() );
    if ( dt ) return dt->GetType() != kOther_t;
    return kFALSE;
}

Bool_t Cppyy::IsComplete( const std::string& type_name )
{  
// verify whether the dictionary of this class is fully available
   Bool_t b = kFALSE;
   
   Int_t oldEIL = gErrorIgnoreLevel;
   gErrorIgnoreLevel = 3000;
   TClass* klass = TClass::GetClass( TClassEdit::ShortType( type_name.c_str(), 1 ).c_str() );
   if ( klass && klass->GetClassInfo() )     // works for normal case w/ dict
      b = gInterpreter->ClassInfo_IsLoaded( klass->GetClassInfo() );
   else {      // special case for forward declared classes
      ClassInfo_t* ci = gInterpreter->ClassInfo_Factory( type_name.c_str() );
      if ( ci ) {
         b = gInterpreter->ClassInfo_IsLoaded( ci );
         gInterpreter->ClassInfo_Delete( ci );    // we own the fresh class info
      }
   }
   gErrorIgnoreLevel = oldEIL;
   return b;    
}  

// memory management ---------------------------------------------------------
Cppyy::TCppObject_t Cppyy::Allocate( TCppType_t klass )
{
   TClassRef& cr = type_from_handle( klass );
   return (TCppObject_t)cr->New();
}

void Cppyy::Deallocate( TCppType_t /* klass */, TCppObject_t /* instance */ )
{
    /* nothing ... deletion is done in Destruct; that is not a requirement,
       but works well enough */
}

void Cppyy::Destruct( TCppType_t klass, TCppObject_t instance )
{
   TClassRef& cr = type_from_handle( klass );
   cr->Destructor( (void*)instance );
}


// method/function dispatching -----------------------------------------------
static inline ClassInfo_t* GetGlobalNamespaceInfo()
{
   static ClassInfo_t* gcl = gInterpreter->ClassInfo_Factory();
   return gcl;
}

static CallFunc_t* GetCallFunc( Cppyy::TCppMethod_t method )
{
   auto icf = g_method2callfunc.find( method );
   if ( icf != g_method2callfunc.end() )
      return icf->second;

   CallFunc_t* callf = nullptr;
   TFunction* func = (TFunction*)method;
   std::string callString = "";

// create, if not cached
   Cppyy::TCppScope_t scope = declaring_scope( method );
   const TClassRef& klass = type_from_handle( scope );
   if ( klass.GetClass() || (func && scope == GLOBAL_HANDLE) ) {
      ClassInfo_t* gcl = klass.GetClass() ? klass->GetClassInfo() : nullptr;
      if ( ! gcl )
         gcl = GetGlobalNamespaceInfo();

      TCollection* method_args = func->GetListOfMethodArgs();
      TIter iarg( method_args );
   
      TMethodArg* method_arg = 0;
      while ((method_arg = (TMethodArg*)iarg.Next())) {
         std::string fullType = method_arg->GetFullTypeName();
         if ( callString.empty() )
            callString = fullType;
         else
            callString += ", " + fullType;
      }

      Long_t offset = 0;
      callf = gInterpreter->CallFunc_Factory();

      gInterpreter->CallFunc_SetFuncProto(
         callf,
         gcl,
         func ? func->GetName() : klass->GetName(),
         callString.c_str(),
         func ? (func->Property() & kIsConstMethod) : kFALSE,
         &offset,
         ROOT::kExactMatch );

// CLING WORKAROUND -- The number of arguments is not always correct (e.g. when there
//                     are default parameters, causing the callString to be wrong and
//                     the exact match to fail); or the method may have been inline or
//                     be compiler generated. In all those cases the exact match fails,
//                     whereas the conversion match sometimes works.
      if ( ! gInterpreter->CallFunc_IsValid( callf ) ) {
         gInterpreter->CallFunc_SetFuncProto(
            callf,
            gcl,
            func ? func->GetName() : klass->GetName(),
            callString.c_str(),
            func ? (func->Property() & kIsConstMethod) : kFALSE,
            &offset );  // <- no kExactMatch as that will fail
      }
// -- CLING WORKAROUND

   }

   if ( !( callf && gInterpreter->CallFunc_IsValid( callf ) ) ) {
      PyErr_Format( PyExc_RuntimeError, "could not resolve %s::%s(%s)",
         const_cast<TClassRef&>(klass).GetClassName(),
         func ? func->GetName() : const_cast<TClassRef&>(klass).GetClassName(),
         callString.c_str() );
      if ( callf ) gInterpreter->CallFunc_Delete( callf );
      return nullptr;
   }

   g_method2callfunc[ method ] = callf;
   return callf;
}

static CallFunc_t* PrepareCall( Cppyy::TCppMethod_t method, void* args_ )
{
   CallFunc_t* callf = GetCallFunc( method );
   if ( callf ) {
      gInterpreter->CallFunc_ResetArg( callf );
      const std::vector<TParameter>& args = *(std::vector<TParameter>*)args_;
      for ( auto arg : args ) {
         switch ( arg.fTypeCode ) {
         case 'd':       /* double */
            gInterpreter->CallFunc_SetArg( callf, arg.fValue.fDouble );
            break;
         case 'k':       /* long long */
            gInterpreter->CallFunc_SetArg( callf, arg.fValue.fLongLong );
            break;
         case 'K':       /* unsigned long long */
            gInterpreter->CallFunc_SetArg( callf, arg.fValue.fULongLong );
            break;
         case 'l':       /* long */
            gInterpreter->CallFunc_SetArg( callf, arg.fValue.fLong );
            break;
         case 'U':       /* unsigned long */
            gInterpreter->CallFunc_SetArg( callf, (ULong64_t)arg.fValue.fULong );
            break;
         case 'v':       /* void* */
            gInterpreter->CallFunc_SetArg( callf, (Long_t)arg.fValue.fVoidp );
            break;
         case 'V':       /* void** */
            gInterpreter->CallFunc_SetArg( callf, (Long_t)arg.fRef );
            break;
         default:
            std::cerr << "unknown type code: " << arg.fTypeCode << std::endl;
            break;
         }
      }

   }

   return callf;
}

void Cppyy::CallV( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return /* TODO ... report error */;
   gInterpreter->CallFunc_Exec( func, (void*)self );
}

UChar_t Cppyy::CallB( TCppMethod_t method, TCppObject_t self, void* args ) {
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (UChar_t)-1;
   return (UChar_t)gInterpreter->CallFunc_ExecInt( func, (void*)self );
}

Char_t Cppyy::CallC( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (Char_t)-1;
   return (Char_t)gInterpreter->CallFunc_ExecInt( func, (void*)self );
}

Short_t Cppyy::CallH( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (Short_t)-1;
   return (Short_t)gInterpreter->CallFunc_ExecInt( func, (void*)self );
}

Int_t Cppyy::CallI( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (Int_t)-1;
   return (Int_t)gInterpreter->CallFunc_ExecInt( func, (void*)self );
}

Long_t Cppyy::CallL( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (Long_t)-1;
// the name ExecInt is historic, the return is Long_t
   return (Long_t)gInterpreter->CallFunc_ExecInt( func, (void*)self );
}

Long64_t Cppyy::CallLL( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (Long64_t)-1;
   return (Long64_t)gInterpreter->CallFunc_ExecInt64( func, (void*)self );
}

Float_t Cppyy::CallF( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (Float_t)-1.f;
   return (Float_t)gInterpreter->CallFunc_ExecDouble( func, (void*)self );
}

Double_t Cppyy::CallD( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return -1.;
   return gInterpreter->CallFunc_ExecDouble( func, (void*)self );
}

void* Cppyy::CallR( TCppMethod_t /* method */, TCppObject_t /* self */, void* /* args */ )
{
   return NULL;
}

Char_t* Cppyy::CallS( TCppMethod_t method, TCppObject_t self, void* args )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (Char_t*)nullptr;
   return (Char_t*)gInterpreter->CallFunc_ExecInt( func, (void*)self );
}

Cppyy::TCppObject_t Cppyy::CallConstructor(
      TCppMethod_t method, TCppType_t /* klass */, void* args ) {
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (TCppObject_t)0;
// the name ExecInt is historic, the return is Long_t
   return (TCppObject_t)gInterpreter->CallFunc_ExecInt( func, nullptr );
}

Cppyy::TCppObject_t Cppyy::CallO( TCppMethod_t method,
      TCppObject_t self, void* args, TCppType_t /* result_type */ )
{
   CallFunc_t* func = PrepareCall( method, args );
   if ( !func ) return (TCppObject_t)0;

   TInterpreterValue* value = gInterpreter->CreateTemporary();
   gInterpreter->CallFunc_Exec( func, self, *value );
   if ( ! value->IsValid() ) {
      delete value;
      return (TCppObject_t)0;
   }

// last time I checked, this was a no-op, but by convention it is required
   gInterpreter->ClearStack();

// need to return as a TInterpreterValue as it does not allow taking ownership
// of just the pointer, so the caller needs to delete the TInterpreterValue to
// delete the held object
   return (TCppObject_t)value;
}

Cppyy::TCppMethPtrGetter_t Cppyy::GetMethPtrGetter(
      TCppScope_t /* scope */, TCppIndex_t /* imeth */ )
{
   return (TCppMethPtrGetter_t)0;
}


// handling of function argument buffer --------------------------------------
void* Cppyy::AllocateFunctionArgs( size_t nargs )
{
   return new TParameter[nargs];
}

void Cppyy::DeallocateFunctionArgs( void* args )
{
   delete [] (TParameter*)args;
}

size_t Cppyy::GetFunctionArgSizeof()
{
   return sizeof( TParameter );
}

size_t Cppyy::GetFunctionArgTypeoffset()\
{
   return offsetof( TParameter, fTypeCode );
}


// scope reflection information ----------------------------------------------
Bool_t Cppyy::IsNamespace( TCppScope_t scope) {
// Test if this scope represents a namespace.
   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() )
      return cr->Property() & kIsNamespace;
   return kFALSE;
}

Bool_t Cppyy::IsAbstract( TCppType_t klass ) {
// Test if this type may not be instantiated.
   TClassRef& cr = type_from_handle( klass );
   if ( cr.GetClass() )
      return cr->Property() & kIsAbstract;
   return kFALSE;
}

Bool_t Cppyy::IsEnum( const std::string& type_name ) {
   return gInterpreter->ClassInfo_IsEnum( type_name.c_str() );
}
    
    
// class reflection information ----------------------------------------------
std::string Cppyy::GetFinalName( TCppType_t klass )
{
   if ( klass == GLOBAL_HANDLE )    // due to CLING WORKAROUND in InitConverters_
      return "";
   // TODO: either this or GetScopedFinalName is wrong
   TClassRef& cr = type_from_handle( klass );
   return ResolveName( cr->GetName() );
}

std::string Cppyy::GetScopedFinalName( TCppType_t klass )
{
   // TODO: either this or GetFinalName is wrong
   TClassRef& cr = type_from_handle( klass );
   return ResolveName( cr->GetName() );
}   

Bool_t Cppyy::HasComplexHierarchy( TCppType_t /* handle */ )
{
// Always TRUE for now (pre-empts certain optimizations).
  return kTRUE;
}

Cppyy::TCppIndex_t Cppyy::GetNumBases( TCppType_t klass )
{
// Get the total number of base classes that this class has.
   TClassRef& cr = type_from_handle( klass );
   if ( cr.GetClass() && cr->GetListOfBases() != 0 )
      return cr->GetListOfBases()->GetSize();
   return 0;
}

std::string Cppyy::GetBaseName( TCppType_t klass, TCppIndex_t ibase )
{
   TClassRef& cr = type_from_handle( klass );
   return ((TBaseClass*)cr->GetListOfBases()->At( ibase ))->GetName();
}

Bool_t Cppyy::IsSubtype( TCppType_t derived, TCppType_t base )
{
   TClassRef& derived_type = type_from_handle( derived );
   TClassRef& base_type = type_from_handle( base );
   return derived_type->GetBaseClass( base_type ) != 0;
}

// type offsets --------------------------------------------------------------
ptrdiff_t Cppyy::GetBaseOffset(
      TCppType_t derived, TCppType_t base, TCppObject_t address, int direction )
{
// calculate offsets between declared and actual type, up-cast: direction > 0; down-cast: direction < 0
   if ( derived == base || !(base && derived) )
      return (ptrdiff_t)0;

   TClassRef& cd = type_from_handle( derived );
   TClassRef& cb = type_from_handle( base );

   if ( !cd.GetClass() || !cb.GetClass() )
      return (ptrdiff_t)0;

   Long_t offset = gInterpreter->ClassInfo_GetBaseOffset(
      cd->GetClassInfo(), cb->GetClassInfo(), (void*)address, direction > 0 );
   if ( offset == -1 ) {
   // warn to allow diagnostics, but 0 offset is often good, so use that and continue
      std::ostringstream msg;
      msg << "failed offset calculation between " << cb->GetName() << " and " << cd->GetName();
      PyErr_Warn( PyExc_RuntimeWarning, const_cast<char*>( msg.str().c_str() ) );
      return 0;
   }

   return (ptrdiff_t)(direction < 0 ? -offset : offset);
}


// method/function reflection information ------------------------------------
Cppyy::TCppIndex_t Cppyy::GetNumMethods( TCppScope_t scope )
{
   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() && cr->GetListOfMethods() ) {
      Cppyy::TCppIndex_t nMethods = (TCppIndex_t)cr->GetListOfMethods()->GetSize();
      if ( nMethods == (TCppIndex_t)0 ) {
         std::string clName = GetScopedFinalName( scope );
         if ( clName.find( '<' ) != std::string::npos ) {
         // chicken-and-egg problem: TClass does not know about methods until instantiation: force it
            if ( TClass::GetClass( ("std::" + clName).c_str() ) )
               clName = "std::" + clName;
            std::ostringstream stmt;
            stmt << "template class " << clName << ";";
            gInterpreter->Declare( stmt.str().c_str() );
         // now reload the methods
            return (TCppIndex_t)cr->GetListOfMethods( kTRUE )->GetSize();
         }
      }
      return nMethods;
   } else if ( scope == (TCppScope_t)GLOBAL_HANDLE ) {
   // enforce lazines by denying the existence of methods
      return (TCppIndex_t)0;
   }
   return (TCppIndex_t)0;
}

Cppyy::TCppIndex_t Cppyy::GetMethodIndexAt( TCppScope_t /* scope */, TCppIndex_t /* imeth */ )
{
   return (TCppIndex_t)0;
}

std::vector< Cppyy::TCppMethod_t > Cppyy::GetMethodsFromName(
      TCppScope_t scope, const std::string& name )
{
// TODO: this method assumes that the call for this name is made only
// once, and thus there is no need to store the results of the search
// in g_globalfuncs ... probably true, but needs verification
   std::vector< TCppMethod_t > methods;
   if ( scope == GLOBAL_HANDLE ) {
      TCollection* funcs = gROOT->GetListOfGlobalFunctions( kTRUE );
      g_globalfuncs.reserve(funcs->GetSize());

      TIter ifunc(funcs);

      TFunction* func = 0;
      while ( (func = (TFunction*)ifunc.Next()) ) {
      // cover not only direct matches, but also template matches
         std::string fn = func->GetName();
         if ( fn.rfind( name, 0 ) == 0 ) {
         // either match exactly, or match the name as template
            if ( (name.size() == fn.size()) ||
                 (name.size() < fn.size() && fn[name.size()] == '<') ) {
               methods.push_back( (TCppMethod_t)func );
            }
         }
      }
   }

   return methods;
}

Cppyy::TCppMethod_t Cppyy::GetMethod( TCppScope_t scope, TCppIndex_t imeth )
{
   TFunction* f = type_get_method( scope, imeth );
   return (Cppyy::TCppMethod_t)f;
}

std::string Cppyy::GetMethodName( TCppMethod_t method )
{
   if ( method ) {
      std::string name = ((TFunction*)method)->GetName();
      //if ( IsMethodTemplate( method ) )
      //   return name.substr( 0, name.find('<') );
      return name;
   }
   return "<unknown>";
}

std::string Cppyy::GetMethodResultType( TCppMethod_t method )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      if ( f->ExtraProperty() & kIsConstructor )
         return "constructor";
      return f->GetReturnTypeName();
   }
   return "<unknown>";
}

Cppyy::TCppIndex_t Cppyy::GetMethodNumArgs( TCppMethod_t method )
{
   if ( method )
      return ((TFunction*)method)->GetNargs();
   return 0;
}

Cppyy::TCppIndex_t Cppyy::GetMethodReqArgs( TCppMethod_t method )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      return (TCppIndex_t)(f->GetNargs() - f->GetNargsOpt());
   }
   return (TCppIndex_t)0;
}

std::string Cppyy::GetMethodArgName( TCppMethod_t method, int iarg )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      TMethodArg* arg = (TMethodArg*)f->GetListOfMethodArgs()->At( iarg );
      return arg->GetName();
   }
   return "<unknown>";
}

std::string Cppyy::GetMethodArgType( TCppMethod_t method, int iarg )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      TMethodArg* arg = (TMethodArg*)f->GetListOfMethodArgs()->At( iarg );
      return arg->GetTypeNormalizedName();
   }
   return "<unknown>";
}

std::string Cppyy::GetMethodArgDefault( TCppMethod_t method, int iarg )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      TMethodArg* arg = (TMethodArg*)f->GetListOfMethodArgs()->At( iarg );
      const char* def = arg->GetDefault();
      if ( def )
         return def;
   }

   return "";
}

std::string Cppyy::GetMethodSignature( TCppScope_t /* scope */, TCppIndex_t /* imeth */ )
{
   return "<unknown>";
}

Bool_t Cppyy::IsConstMethod( TCppMethod_t method )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      return f->Property() & kIsConstMethod;
   }
   return kFALSE;
}


Bool_t Cppyy::IsMethodTemplate( TCppMethod_t method )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      std::string name = f->GetName();
      return (name[name.size()-1] == '>') && (name.find('<') != std::string::npos);
   }
   return kFALSE;
}

Cppyy::TCppIndex_t Cppyy::GetMethodNumTemplateArgs(
      TCppScope_t /* scope */, TCppIndex_t /* imeth */ )
{
   return (TCppIndex_t)0;
}

std::string Cppyy::GetMethodTemplateArgName(
      TCppScope_t /* scope */, TCppIndex_t /* imeth */, TCppIndex_t /* iarg */ )
{
   return "<unknown>";
}

Cppyy::TCppIndex_t Cppyy::GetGlobalOperator(
      TCppScope_t /* scope */, TCppType_t /* lc */, TCppType_t /* rc */, const std::string& /* op */ )
{
   return (TCppIndex_t)0;
}

// method properties ---------------------------------------------------------
Bool_t Cppyy::IsConstructor( TCppMethod_t method )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      return f->ExtraProperty() & kIsConstructor;
   }
   return kFALSE;
}

Bool_t Cppyy::IsPublicMethod( TCppMethod_t method )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      return f->Property() & kIsPublic;
   }
   return kFALSE;
}

Bool_t Cppyy::IsStaticMethod( TCppMethod_t method )
{
   if ( method ) {
      TFunction* f = (TFunction*)method;
      return f->Property() & kIsStatic;
   }
   return kFALSE;
}

// data member reflection information ----------------------------------------
Cppyy::TCppIndex_t Cppyy::GetNumDatamembers( TCppScope_t scope )
{
   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() && cr->GetListOfDataMembers() )
      return cr->GetListOfDataMembers()->GetSize();
   else if ( scope == (TCppScope_t)GLOBAL_HANDLE ) {
      std::cerr << " global data should be retrieved lazily " << std::endl;
      TCollection* vars = gROOT->GetListOfGlobals( kTRUE ); 
      if ( g_globalvars.size() != (GlobalVars_t::size_type)vars->GetSize() ) {
         g_globalvars.clear();
         g_globalvars.reserve(vars->GetSize());

         TIter ivar(vars);

         TGlobal* var = 0;
         while ( (var = (TGlobal*)ivar.Next()) )
            g_globalvars.push_back( var );
      }
      return (TCppIndex_t)g_globalvars.size();
   }
   return (TCppIndex_t)0;
}

std::string Cppyy::GetDatamemberName( TCppScope_t scope, TCppIndex_t idata )
{
   TClassRef& cr = type_from_handle( scope );
   if (cr.GetClass()) {
      TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
      return m->GetName();
   }
   assert( scope == (TCppScope_t)GLOBAL_HANDLE );
   TGlobal* gbl = g_globalvars[ idata ];
   return gbl->GetName();
}

std::string Cppyy::GetDatamemberType( TCppScope_t scope, TCppIndex_t idata )
{
   if ( scope == GLOBAL_HANDLE ) {
      TGlobal* gbl = g_globalvars[ idata ];
      std::string fullType = gbl->GetFullTypeName();
      if ( fullType[fullType.size()-1] == '*' && \
           fullType.find( "char", 0, 4 ) == std::string::npos )
         fullType.append( "*" );
      else if ( (int)gbl->GetArrayDim() > 1 )
         fullType.append( "*" );
      else if ( (int)gbl->GetArrayDim() == 1 ) {
         std::ostringstream s;
         s << '[' << gbl->GetMaxIndex( 0 ) << ']' << std::ends;
         fullType.append( s.str() );
      }
      return fullType;
   }

   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() )  {
      TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
      std::string fullType = m->GetFullTypeName();
      if ( (int)m->GetArrayDim() > 1 || (!m->IsBasic() && m->IsaPointer()) )
         fullType.append( "*" );
      else if ( (int)m->GetArrayDim() == 1 ) {
         std::ostringstream s;
         s << '[' << m->GetMaxIndex( 0 ) << ']' << std::ends;
         fullType.append( s.str() );
      }
      return fullType;
   }

   return "<unknown>";
}

ptrdiff_t Cppyy::GetDatamemberOffset( TCppScope_t scope, TCppIndex_t idata )
{
   if ( scope == GLOBAL_HANDLE ) {
      TGlobal* gbl = g_globalvars[ idata ];
      return (ptrdiff_t)gbl->GetAddress();
   }

   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() ) {
      TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
      return (ptrdiff_t)m->GetOffsetCint();      // yes, CINT ...
   }

   return (ptrdiff_t)0;
}

Cppyy::TCppIndex_t Cppyy::GetDatamemberIndex( TCppScope_t scope, const std::string& name )
{
   if ( scope == GLOBAL_HANDLE ) {
      TGlobal* gb = (TGlobal*)gROOT->GetListOfGlobals( kTRUE )->FindObject( name.c_str() );
      if ( gb && gb->GetAddress() && gb->GetAddress() != (void*)-1 ) {
         g_globalvars.push_back( gb );
         return g_globalvars.size() - 1;
      }

   /*
      ClassInfo_t* gbl = gInterpreter->ClassInfo_Factory( "" ); // "" == global namespace
      DataMemberInfo_t* dt = gInterpreter->DataMemberInfo_Factory( gbl );
      while ( gInterpreter->DataMemberInfo_Next( dt ) ) {
         if ( gInterpreter->DataMemberInfo_IsValid( dt ) &&
            gInterpreter->DataMemberInfo_Name( dt ) == name ) {
            // store and return ...
         }
      }
   */
   } else {
      TClassRef& cr = type_from_handle( scope );
      if ( cr.GetClass() ) {
         TDataMember* dm =
            (TDataMember*)cr->GetListOfDataMembers()->FindObject( name.c_str() );
         // TODO: turning this into an index is silly ...
         if ( dm ) return (TCppIndex_t)cr->GetListOfDataMembers()->IndexOf( dm );
      }
   }

   return (TCppIndex_t)-1;
}


// data member properties ----------------------------------------------------
Bool_t Cppyy::IsPublicData( TCppScope_t scope, TCppIndex_t idata )
{
   if ( scope == GLOBAL_HANDLE )
      return kTRUE;
   TClassRef& cr = type_from_handle( scope );
   if ( cr->Property() & kIsNamespace )
      return kTRUE;
   TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
   return m->Property() & kIsPublic;
}

Bool_t Cppyy::IsStaticData( TCppScope_t scope, TCppIndex_t idata  )
{
   if ( scope == GLOBAL_HANDLE )
      return kTRUE;
   TClassRef& cr = type_from_handle( scope );
   if ( cr->Property() & kIsNamespace )
      return kTRUE;
   TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
   return m->Property() & kIsStatic;
}

Bool_t Cppyy::IsConstData( TCppScope_t scope, TCppIndex_t idata )
{
   if ( scope == GLOBAL_HANDLE ) {
      TGlobal* gbl = g_globalvars[ idata ];
      return gbl->Property() & kIsConstant;
   }
   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() ) {
      TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
      return m->Property() & kIsConstant;
   }
   return kFALSE;
}

Bool_t Cppyy::IsEnumData( TCppScope_t scope, TCppIndex_t idata )
{
   if ( scope == GLOBAL_HANDLE ) {
      TGlobal* gbl = g_globalvars[ idata ];
      return gbl->Property() & kIsEnum;
   }
   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() ) {
      TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
      return m->Property() & kIsEnum;
   }
   return kFALSE;
}

Int_t Cppyy::GetDimensionSize( TCppScope_t scope, TCppIndex_t idata, int dimension )
{
   if ( scope == GLOBAL_HANDLE ) {
      TGlobal* gbl = g_globalvars[ idata ];
      return gbl->GetMaxIndex( dimension );
   }
   TClassRef& cr = type_from_handle( scope );
   if ( cr.GetClass() ) {
      TDataMember* m = (TDataMember*)cr->GetListOfDataMembers()->At( idata );
      return m->GetMaxIndex( dimension );
   }
   return (Int_t)-1;
}
