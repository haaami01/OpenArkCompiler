/*
 * Copyright (c) [2019] Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
 */
#include "gen_check_cast.h"
#include <iostream>
#include <algorithm>
#include "reflection_analysis.h"
#include "mir_lower.h"

// This phase does two things:
// #1 implement the opcode check-cast vx, type_id
//    according to the check-cast definition:
//    Checks whether the object reference in vx can be cast
//    to an instance of a class referenced by type_id.
//    Throws ClassCastException if the cast is not possible, continues execution otherwise.
//    in our case check if object can be cast or insert MCC_Reflect_ThrowCastException
//    before the stmt.
// #2 optimise instance-of && cast

namespace maple {
CheckCastGenerator::CheckCastGenerator(MIRModule *mod, KlassHierarchy *kh, bool dump)
    : FuncOptimizeImpl(mod, kh, dump) {
  InitTypes();
  InitFuncs();
}


void CheckCastGenerator::InitTypes() {
  pointerObjType = GlobalTables::GetTypeTable().GetOrCreatePointerType(WKTypes::Util::GetJavaLangObjectType());
  classinfoType = GlobalTables::GetTypeTable().GetOrCreateClassType(NameMangler::kClassMetadataTypeName, GetModule());
  pointerClassMetaType = GlobalTables::GetTypeTable().GetOrCreatePointerType(classinfoType);
}

void CheckCastGenerator::InitFuncs() {
  throwCastException = builder->GetOrCreateFunction(kMCCReflectThrowCastException, TyIdx(PTY_void));
  throwCastException->SetAttr(FUNCATTR_nosideeffect);
  checkCastingNoArray = builder->GetOrCreateFunction(kMCCReflectCheckCastingNoArray, TyIdx(PTY_void));
  checkCastingNoArray->SetAttr(FUNCATTR_nosideeffect);
  checkCastingArray = builder->GetOrCreateFunction(kMCCReflectCheckCastingArray, TyIdx(PTY_void));
  checkCastingArray->SetAttr(FUNCATTR_nosideeffect);

}

MIRSymbol *CheckCastGenerator::GetOrCreateClassInfoSymbol(const std::string &className) {
  std::string classInfoName = CLASSINFO_PREFIX_STR + className;
  MIRSymbol *classInfoSymbol = builder->GetGlobalDecl(classInfoName.c_str());
  if (classInfoSymbol == nullptr) {
    GStrIdx gStrIdx = GlobalTables::GetStrTable().GetStrIdxFromName(className);
    MIRType *classType =
        GlobalTables::GetTypeTable().GetTypeFromTyIdx(GlobalTables::GetTypeNameTable().GetTyidxFromGstrIdx(gStrIdx));
    MIRStorageClass sclass = (classType && static_cast<MIRClassType*>(classType)->IsLocal()) ? kScGlobal : kScExtern;
    // Creating global symbol needs synchronization.
    classInfoSymbol = builder->CreateGlobalDecl(classInfoName.c_str(), GlobalTables::GetTypeTable().GetPtr(), sclass);
  }
  return classInfoSymbol;
}

void CheckCastGenerator::GenCheckCast(BaseNode *stmt) {
  // Handle the special case like (Type)null, we don't need a checkcast.
  if (stmt->GetOpCode() == OP_intrinsiccallwithtypeassigned) {
    IntrinsiccallNode *callNode = static_cast<IntrinsiccallNode*>(stmt);
    ASSERT(callNode->GetNopndSize() == 1, "array size error");
    BaseNode *opnd = callNode->Opnd(0);
    if (opnd->GetOpCode() == OP_constval) {
      const size_t callNodeNretsSize = callNode->GetReturnVec().size();
      CHECK_FATAL(callNodeNretsSize > 0, "container check");
      CallReturnPair callReturnPair = callNode->GetReturnVec()[0];
      StmtNode *assignReturnTypeNode = nullptr;
      if (!callReturnPair.second.IsReg()) {
        assignReturnTypeNode =
            builder->CreateStmtDassign(callReturnPair.first, callReturnPair.second.GetFieldID(), opnd);
      } else {
        PregIdx pregIdx = callReturnPair.second.GetPregIdx();
        MIRPreg *mirPreg = currFunc->GetPregTab()->PregFromPregIdx(pregIdx);
        assignReturnTypeNode = builder->CreateStmtRegassign(mirPreg->GetPrimType(), pregIdx, opnd);
      }
      currFunc->GetBody()->ReplaceStmt1WithStmt2(static_cast<StmtNode*>(stmt), assignReturnTypeNode);
      return;
    }
  }
  // Do type check first.
  IntrinsiccallNode *callNode = static_cast<IntrinsiccallNode*>(stmt);
  ASSERT(callNode->GetNopndSize() == 1, "array size error");
  TyIdx checkTyidx = callNode->GetTyIdx();
  MIRType *checkType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(checkTyidx);
  Klass *checkKlass = klassHierarchy->GetKlassFromTyIdx(static_cast<MIRPtrType*>(checkType)->GetPointedTyIdx());

  {
    if (checkKlass && strcmp("", checkKlass->GetKlassName().c_str())) {
      if (!strcmp(checkKlass->GetKlassName().c_str(), NameMangler::kJavaLangObjectStr)) {
        const size_t callNodeNopndSize1 = callNode->GetNopndSize();
        CHECK_FATAL(callNodeNopndSize1 > 0, "container check");
        if (callNode->GetNopndAt(0)->GetPrimType() != PTY_ref && callNode->GetNopndAt(0)->GetPrimType() != PTY_ptr) {
          // If source = Ljava_2Flang_2FObject_3B, sub = primitive type then throw CastException.
          MIRSymbol *classSt = GetOrCreateClassInfoSymbol(checkKlass->GetKlassName());
          BaseNode *valueExpr = builder->CreateExprAddrof(0, classSt);
          MapleVector<BaseNode*> args(builder->GetCurrentFuncCodeMpAllocator()->Adapter());
          args.push_back(valueExpr);
          args.push_back(callNode->GetNopndAt(0));
          args.push_back(builder->CreateIntConst(0, PTY_ptr));
          StmtNode *dassignStmt = builder->CreateStmtCall(throwCastException->GetPuidx(), args);
          currFunc->GetBody()->InsertBefore(static_cast<StmtNode*>(stmt), dassignStmt);
        }
      } else {
        MIRSymbol *classSt = GetOrCreateClassInfoSymbol(checkKlass->GetKlassName());
        BaseNode *valueExpr = builder->CreateExprAddrof(0, classSt);
        BaseNode *nullPtr = builder->CreateIntConst(0, PTY_ptr);
        const size_t callNodeNopndSize2 = callNode->GetNopndSize();
        CHECK_FATAL(callNodeNopndSize2 > 0, "container check");
        BaseNode *cond =
            builder->CreateExprCompare(OP_ne, GlobalTables::GetTypeTable().GetUInt1(),
                                       GlobalTables::GetTypeTable().GetPtr(), callNode->GetNopndAt(0), nullPtr);
        IfStmtNode *ifStmt = static_cast<IfStmtNode*>(builder->CreateStmtIf(cond));
        MIRType *mVoidPtr = GlobalTables::GetTypeTable().GetVoidPtr();
        CHECK_FATAL(mVoidPtr != nullptr, "builder->GetVoidPtr() is null in CheckCastGenerator::GenCheckCast");
        BaseNode *opnd = callNode->GetNopndAt(0);
        BaseNode *ireadExpr = GetObjectShadow(opnd);

        BaseNode *innerCond = builder->CreateExprCompare(OP_ne, GlobalTables::GetTypeTable().GetUInt1(),
                                                         GlobalTables::GetTypeTable().GetPtr(), valueExpr, ireadExpr);
        IfStmtNode *innerIfStmt = static_cast<IfStmtNode *>(builder->CreateStmtIf(innerCond));
        MapleVector<BaseNode*> args(builder->GetCurrentFuncCodeMpAllocator()->Adapter());
        args.push_back(valueExpr);
        args.push_back(opnd);
        StmtNode *dassignStmt = builder->CreateStmtCall(checkCastingNoArray->GetPuidx(), args);
        innerIfStmt->GetThenPart()->AddStatement(dassignStmt);
        ifStmt->GetThenPart()->AddStatement(innerIfStmt);
        currFunc->GetBody()->InsertBefore(static_cast<StmtNode*>(stmt), ifStmt);
      }
    } else {
      MIRType *pointedType =
          GlobalTables::GetTypeTable().GetTypeFromTyIdx(static_cast<MIRPtrType*>(checkType)->GetPointedTyIdx());
      if (pointedType->GetKind() == kTypeJArray) {
        // Java array.
        MIRJarrayType *jarrayType = static_cast<MIRJarrayType*>(pointedType);
        std::string arrayName = jarrayType->GetJavaName();
        int dim = 0;
        while (arrayName[dim] == 'A') {
          dim++;
        }
        MIRSymbol *elemClassSt = nullptr;
        std::string elementName = arrayName.substr(dim, arrayName.size() - dim);
        MIRType *mVoidPtr2 = GlobalTables::GetTypeTable().GetVoidPtr();
        CHECK_FATAL(mVoidPtr2 != nullptr, "null ptr check");
        if (elementName == "I" || elementName == "F" || elementName == "B" || elementName == "C" ||
            elementName == "S" || elementName == "J" || elementName == "D" || elementName == "Z" ||
            elementName == "V") {
          std::string primClassinfoName = PRIMITIVECLASSINFO_PREFIX_STR + elementName;
          elemClassSt = builder->GetGlobalDecl(primClassinfoName.c_str());
          if (elemClassSt == nullptr) {
            elemClassSt = builder->CreateGlobalDecl(primClassinfoName.c_str(), GlobalTables::GetTypeTable().GetPtr());
          }
        } else {
          elemClassSt = GetOrCreateClassInfoSymbol(elementName);
        }
        BaseNode *valueExpr = builder->CreateExprAddrof(0, elemClassSt);
        UStrIdx stridx = GlobalTables::GetUStrTable().GetOrCreateStrIdxFromName(jarrayType->GetJavaName());
        ConststrNode *signatureNode = currFunc->GetCodeMempool()->New<ConststrNode>(stridx);
        signatureNode->SetPrimType(PTY_ptr);
        MapleVector<BaseNode*> opnds(currFunc->GetCodeMempoolAllocator()->Adapter());
        opnds.push_back(valueExpr);
        const size_t callNodeNopndSize3 = callNode->GetNopndSize();
        CHECK_FATAL(callNodeNopndSize3 > 0, "container check");
        opnds.push_back(callNode->GetNopndAt(0));
        opnds.push_back(builder->CreateIntConst(dim, PTY_ptr));
        opnds.push_back(signatureNode);
        StmtNode *dassignStmt = builder->CreateStmtCall(checkCastingArray->GetPuidx(), opnds);
        currFunc->GetBody()->InsertBefore(static_cast<StmtNode*>(stmt), dassignStmt);
      } else {
        MIRTypeKind kd = pointedType->GetKind();
        if (kd == kTypeStructIncomplete || kd == kTypeClassIncomplete || kd == kTypeInterfaceIncomplete) {
          LogInfo::MapleLogger() << "Warining: CheckCastGenerator::GenCheckCast "
                                 << GlobalTables::GetStrTable().GetStringFromStrIdx(pointedType->GetNameStrIdx())
                                 << " INCOMPLETE " << std::endl;
        } else {
          CHECK_FATAL(false, "unsupport kind");
        }
      }
    }
  }
  if (callNode->GetOpCode() == OP_intrinsiccallwithtype) {
    return;
  }
  BaseNode *opnd = callNode->Opnd(0);
  ASSERT(opnd->GetOpCode() == OP_dread || opnd->GetOpCode() == OP_regread || opnd->GetOpCode() == OP_iread ||
         opnd->GetOpCode() == OP_retype, "unknown calltype! check it!");

  MIRType *fromType = nullptr;
  if (opnd->GetOpCode() == OP_dread) {
    fromType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(
        currFunc->GetLocalOrGlobalSymbol(static_cast<AddrofNode*>(opnd)->GetStIdx())->GetTyIdx());
  } else if (opnd->GetOpCode() == OP_retype) {
    fromType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(static_cast<RetypeNode*>(opnd)->GetTyIdx());
  } else if (opnd->GetOpCode() == OP_iread) {
    IreadNode *irnode = static_cast<IreadNode*>(opnd);
    MIRPtrType *ptrType = static_cast<MIRPtrType*>(GlobalTables::GetTypeTable().GetTypeFromTyIdx(irnode->GetTyIdx()));
    if (irnode->GetFieldID() != 0) {
      MIRType *pointedType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(ptrType->GetPointedTyIdx());
      MIRStructType *structType = nullptr;
      if (pointedType->GetKind() != kTypeJArray) {
        structType = static_cast<MIRStructType*>(pointedType);
      } else {
        // It's a Jarray type. Using it's parent's field info: java.lang.Object.
        structType = static_cast<MIRJarrayType*>(pointedType)->GetParentType();
      }
      CHECK_FATAL(structType, "null ptr check");
      fromType = structType->GetFieldType(irnode->GetFieldID());
    } else {
      fromType = GlobalTables::GetTypeTable().GetTypeFromTyIdx(ptrType->GetPointedTyIdx());
    }
  } else if (opnd->GetOpCode() == OP_regread) {
    RegreadNode *regReadNode = static_cast<RegreadNode*>(opnd);
    MIRPreg *mirPreg = currFunc->GetPregTab()->PregFromPregIdx(regReadNode->GetRegIdx());
    CHECK_FATAL(mirPreg->GetPrimType() == PTY_ref || mirPreg->GetPrimType() == PTY_ptr,
                "must be reference or ptr type for preg");
    if (opnd->GetPrimType() == PTY_ref) {
      fromType = mirPreg->GetMIRType();
    } else {
      const size_t gTypeTableSize = GlobalTables::GetTypeTable().GetTypeTable().size();
      CHECK_FATAL(gTypeTableSize > PTY_ptr, "container check");
      fromType = GlobalTables::GetTypeTable().GetTypeTable()[PTY_ptr];
    }
  }
  ASSERT((fromType->GetPrimType() == maple::PTY_ptr || fromType->GetPrimType() == maple::PTY_ref),
         "unknown fromType! check it!");
  ASSERT(GlobalTables::GetTypeTable().GetTypeFromTyIdx(callNode->GetTyIdx())->GetPrimType() == maple::PTY_ptr ||
          GlobalTables::GetTypeTable().GetTypeFromTyIdx(callNode->GetTyIdx())->GetPrimType() == maple::PTY_ref,
         "unknown fromType! check it!");
  const size_t callNodeNretSize1 = callNode->GetReturnVec().size();
  CHECK_FATAL(callNodeNretSize1 > 0, "container check");
  CallReturnPair callReturnPair = callNode->GetReturnVec()[0];
  StmtNode *assignReturnTypeNode = nullptr;
  if (!callReturnPair.second.IsReg()) {
    assignReturnTypeNode = builder->CreateStmtDassign(callReturnPair.first, callReturnPair.second.GetFieldID(), opnd);
  } else {
    PregIdx pregIdx = callReturnPair.second.GetPregIdx();
    MIRPreg *mirPreg = currFunc->GetPregTab()->PregFromPregIdx(pregIdx);
    assignReturnTypeNode = builder->CreateStmtRegassign(mirPreg->GetPrimType(), pregIdx, opnd);
  }
  currFunc->GetBody()->ReplaceStmt1WithStmt2(static_cast<StmtNode*>(stmt), assignReturnTypeNode);
}

void CheckCastGenerator::GenAllCheckCast() {
  auto &stmtNodes = currFunc->GetBody()->GetStmtNodes();
  for (auto &stmt : stmtNodes) {
    if (stmt.GetOpCode() == OP_intrinsiccallwithtypeassigned || stmt.GetOpCode() == OP_intrinsiccallwithtype) {
      IntrinsiccallNode &callNode = static_cast<IntrinsiccallNode&>(stmt);
      if (callNode.GetIntrinsic() == INTRN_JAVA_CHECK_CAST) {
        GenCheckCast(&stmt);
        if (stmt.GetOpCode() == OP_intrinsiccallwithtype) {
          currFunc->GetBody()->RemoveStmt(&stmt);
        }
      }
    }
  }
}

BaseNode *CheckCastGenerator::GetObjectShadow(BaseNode *opnd) {
  FieldID fieldID = builder->GetStructFieldIDFromFieldNameParentFirst(WKTypes::Util::GetJavaLangObjectType(),
                                                                      NameMangler::kShadowClassName);
  BaseNode *ireadExpr = builder->CreateExprIread(GlobalTables::GetTypeTable().GetPtr(), pointerObjType, fieldID, opnd);
  return ireadExpr;
}


void CheckCastGenerator::ProcessFunc(MIRFunction *func) {
  if (func->IsEmpty()) {
    return;
  }
  SetCurrentFunction(func);
  GenAllCheckCast();
  MIRLower mirlowerer(*(GetModule()), func);
  mirlowerer.LowerFunc(func);
}
}  // namespace maple
