// KeyPointsCollector.cpp
// ~~~~~~~~~~~~~~~~~~~~~~
// Implementation of KeyPointsCollector interface.
#include "KeyPointsCollector.h"
#include "Common.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

// Ctor Implementation
KeyPointsCollector::KeyPointsCollector(const std::string &filename, bool debug)
    : filename(std::move(filename)), debug(debug) {

  // Check if file exists
  std::ifstream file(filename);
  if (file.good()) {
    file.close();

    // If good, try to parse the translation unit.
    translationUnit =
        clang_parseTranslationUnit(KPCIndex, filename.c_str(), nullptr, 0,
                                   nullptr, 0, CXTranslationUnit_None);
    // Check if parsed properly
    if (translationUnit == nullptr) {
      std::cerr
          << "There was an error parsing the translation unit! Exiting...\n";
      exit(EXIT_FAILURE);
    }
    std::cout << "Translation unit for file: " << filename
              << " successfully parsed.\n";

    // Init cursor and branch count
    rootCursor = clang_getTranslationUnitCursor(translationUnit);
    cxFile = clang_getFile(translationUnit, filename.c_str());
    branchCount = 0;
    executeToolchain();
    // Traverse
  } else {

    std::cerr << "File with name: " << filename
              << ", does not exist! Exiting...\n";
    exit(EXIT_FAILURE);
  }
}

KeyPointsCollector::~KeyPointsCollector() {
  clang_disposeTranslationUnit(translationUnit);
  clang_disposeIndex(KPCIndex);
}

bool KeyPointsCollector::isBranchPointOrFunctionPtr(const CXCursorKind K) {
  switch (K) {
  case CXCursor_IfStmt:
  case CXCursor_ForStmt:
  case CXCursor_DoStmt:
  case CXCursor_WhileStmt:
  case CXCursor_SwitchStmt:
  case CXCursor_CallExpr:
    return true;
  default:
    return false;
  }
}

bool KeyPointsCollector::checkChildAgainstStackTop(CXCursor child) {
  //
  unsigned childLineNum;
  unsigned childColNum;
  BranchPointInfo *currBranch = getCurrentBranch();
  CXSourceLocation childLoc = clang_getCursorLocation(child);
  clang_getSpellingLocation(childLoc, getCXFile(), &childLineNum, &childColNum,
                            nullptr);

  if (childLineNum > currBranch->compoundEndLineNum ||
      (childLineNum == currBranch->compoundEndLineNum &&
       childColNum > currBranch->compoundEndColumnNum)) {
    getCurrentBranch()->addTarget(childLineNum);
    if (debug) {
      printFoundTargetPoint();
    }
    return true;
  } else {
    return false;
  }
}

CXChildVisitResult KeyPointsCollector::VisitorFunctionCore(CXCursor current,
                                                           CXCursor parent,
                                                           CXClientData kpc) {
  // Retrieve required data from call
  KeyPointsCollector *instance = static_cast<KeyPointsCollector *>(kpc);
  const CXCursorKind currKind = clang_getCursorKind(current);
  const CXCursorKind parrKind = clang_getCursorKind(parent);

  // If it is a call expression, recurse on children with special visitor
  if (currKind == CXCursor_DeclRefExpr) {
    clang_visitChildren(parent, &KeyPointsCollector::VisitCallExpr, kpc);
    return CXChildVisit_Continue;
  }

  // If parent a branch point, and current is a compount statement,
  // warm up the KPC for analysis of said branch.
  if (instance->isBranchPointOrFunctionPtr(parrKind) &&
      currKind == CXCursor_CompoundStmt) {

    // Push new point to the stack and retrieve location
    instance->addCursor(parent);
    instance->pushNewBranchPoint();
    CXSourceLocation loc = clang_getCursorLocation(parent);
    clang_getSpellingLocation(loc, instance->getCXFile(),
                              instance->getCurrentBranch()->getBranchPointOut(),
                              nullptr, nullptr);

    // Debug routine
    if (instance->debug) {
      instance->printFoundBranchPoint(parrKind);
    }

    // Visit first child of compound to get target.
    clang_visitChildren(current, &KeyPointsCollector::VisitCompoundStmt, kpc);

    // Save end of compound statement
    BranchPointInfo *currBranch = instance->getCurrentBranch();
    CXSourceLocation compoundEnd =
        clang_getRangeEnd(clang_getCursorExtent(current));
    clang_getSpellingLocation(compoundEnd, instance->getCXFile(),
                              &(currBranch->compoundEndLineNum), nullptr,
                              nullptr);
  }

  // Check to see if child is after the current saved compound statement end '}'
  // location, add to completed.
  if (instance->compoundStmtFoundYet() &&
      instance->getCurrentBranch()->compoundEndLineNum != 0 &&
      instance->checkChildAgainstStackTop(current)) {
    instance->addCompletedBranch();
  }

  // If check to see if it is a FuncDecl
  if (currKind == CXCursor_FunctionDecl) {
    clang_visitChildren(current, &KeyPointsCollector::VisitFuncDecl, kpc);
  }

  // If check to see if it is a VarDecl
  if (currKind == CXCursor_VarDecl) {
    clang_visitChildren(parent, &KeyPointsCollector::VisitVarDecl, kpc);
    return CXChildVisit_Continue;
  }

  return CXChildVisit_Recurse;
}

CXChildVisitResult KeyPointsCollector::VisitCompoundStmt(CXCursor current,
                                                         CXCursor parent,
                                                         CXClientData kpc) {
  KeyPointsCollector *instance = static_cast<KeyPointsCollector *>(kpc);
  const CXCursorKind currKind = clang_getCursorKind(current);
  const CXCursorKind parrKind = clang_getCursorKind(parent);
  if (parrKind != CXCursor_CompoundStmt) {
    std::cerr << "Compound statement visitor called when cursor is not "
                 "compound stmt!\n";
    exit(EXIT_FAILURE);
  }
  // Get line number of first child
  unsigned targetLineNumber;
  CXSourceLocation loc = clang_getCursorLocation(current);
  clang_getSpellingLocation(loc, instance->getCXFile(), &targetLineNumber,
                            nullptr, nullptr);

  // Append line number to targets
  instance->getCurrentBranch()->addTarget(targetLineNumber);
  if (instance->debug) {
    instance->printFoundTargetPoint();
  }
  return CXChildVisit_Continue;
}

CXChildVisitResult KeyPointsCollector::VisitCallExpr(CXCursor current,
                                                     CXCursor parent,
                                                     CXClientData kpc) {
  KeyPointsCollector *instance = static_cast<KeyPointsCollector *>(kpc);

  CXSourceLocation callExprLoc = clang_getCursorLocation(current);

  // Get token and its spelling
  CXToken *varDeclToken = clang_getToken(instance->getTU(), callExprLoc);
  std::string callee =
      CXSTR(clang_getTokenSpelling(instance->getTU(), *varDeclToken));
  QKDBG("TESTING");
  QKDBG(callee);

  return CXChildVisit_Recurse;
}

CXChildVisitResult KeyPointsCollector::VisitVarDecl(CXCursor current,
                                                    CXCursor parent,
                                                    CXClientData kpc) {
  KeyPointsCollector *instance = static_cast<KeyPointsCollector *>(kpc);

  // First retrive the line number
  unsigned varDeclLineNum;
  CXSourceLocation varDeclLoc = clang_getCursorLocation(current);
  clang_getSpellingLocation(varDeclLoc, instance->getCXFile(), &varDeclLineNum,
                            nullptr, nullptr);

  // Get token and its spelling
  CXToken *varDeclToken = clang_getToken(instance->getTU(), varDeclLoc);
  std::string varName =
      CXSTR(clang_getTokenSpelling(instance->getTU(), *varDeclToken));

  // Get reference to map for checking
  std::map<std::string, unsigned> varMap = instance->getVarDecls();

  // Add to map of FuncDecls
  if (varMap.find(varName) == varMap.end()) {
    if (instance->debug) {
      std::cout << "Found VarDecl: " << varName << " at line # "
                << varDeclLineNum << '\n';
    }
    instance->addVarDeclToMap(varName, varDeclLineNum);
  }
  clang_disposeTokens(instance->getTU(), varDeclToken, 1);
  return CXChildVisit_Break;
}

CXChildVisitResult KeyPointsCollector::VisitFuncDecl(CXCursor current,
                                                     CXCursor parent,
                                                     CXClientData kpc) {
  KeyPointsCollector *instance = static_cast<KeyPointsCollector *>(kpc);

  // Get return type, beginning and end loc, and name.
  if (clang_getCursorKind(parent) == CXCursor_FunctionDecl) {
    CXType funcReturnType = clang_getResultType(clang_getCursorType(parent));

    CXString funcReturnTypeSpelling = clang_getTypeSpelling(funcReturnType);
    // Extent
    unsigned begLineNum, endLineNum;
    CXSourceRange funcRange = clang_getCursorExtent(parent);
    CXSourceLocation funcBeg = clang_getRangeStart(funcRange);
    CXSourceLocation funcEnd = clang_getRangeEnd(funcRange);
    clang_getSpellingLocation(funcBeg, instance->getCXFile(), &begLineNum,
                              nullptr, nullptr);
    clang_getSpellingLocation(funcEnd, instance->getCXFile(), &endLineNum,
                              nullptr, nullptr);

    // Get name
    CXToken *funcDeclToken =
        clang_getToken(instance->getTU(), clang_getCursorLocation(parent));
    std::string funcName =
        CXSTR(clang_getTokenSpelling(instance->getTU(), *funcDeclToken));

    // Add to map
    instance->addFuncDecl(begLineNum,
                          std::make_shared<FunctionDeclInfo>(
                              begLineNum, endLineNum, funcName,
                              clang_getCString(funcReturnTypeSpelling)));
    if (instance->debug) {
      std::cout << "Found FunctionDecl: " << funcName << " of return type: "
                << clang_getCString(funcReturnTypeSpelling)
                << " on line #: " << begLineNum << '\n';
    }
    clang_disposeTokens(instance->getTU(), funcDeclToken, 1);
    clang_disposeString(funcReturnTypeSpelling);
  }

  return CXChildVisit_Break;
}

void KeyPointsCollector::collectCursors() {
  clang_visitChildren(rootCursor, this->VisitorFunctionCore, this);
  // Reverse BP list as they were popped in reverse order
  std::reverse(branchPoints.begin(), branchPoints.end());
  addBranchesToDictionary();
}

void KeyPointsCollector::printFoundBranchPoint(const CXCursorKind K) {
  std::cout << "Found branch point: " << CXSTR(clang_getCursorKindSpelling(K))
            << " at line#: " << getCurrentBranch()->branchPoint << '\n';
}

void KeyPointsCollector::printFoundTargetPoint() {
  BranchPointInfo *currentBranch = getCurrentBranch();
  std::cout << "Found target for line branch #: " << currentBranch->branchPoint
            << " at line#: " << currentBranch->targetLineNumbers.back() << '\n';
}

void KeyPointsCollector::printCursorKind(const CXCursorKind K) {
  std::cout << "Found cursor: " << CXSTR(clang_getCursorKindSpelling(K))
            << '\n';
}

void KeyPointsCollector::createDictionaryFile() {

  // Open new file for the dicitonary.
  std::ofstream dictFile(std::string(OUT_DIR + filename + ".branch_dict"));
  dictFile << "Branch Dictionary for: " << filename << '\n';
  dictFile << "-----------------------" << std::string(filename.size(), '-')
           << '\n';

  // Get branch dict ref
  const std::map<unsigned, std::map<unsigned, std::string>> &branchDict =
      getBranchDictionary();

  // Iterate over branch poitns and their targets
  for (const std::pair<unsigned, std::map<unsigned, std::string>> &BP :
       branchDict) {
    for (const std::pair<unsigned, std::string> &targets : BP.second) {
      dictFile << targets.second << ": " << filename << ", " << BP.first << ", "
               << targets.first << '\n';
    }
  }

  // Close file
  dictFile.close();
}

void KeyPointsCollector::addCompletedBranch() {
  branchPoints.push_back(branchPointStack.top());
  branchPointStack.pop();
}

void KeyPointsCollector::addBranchesToDictionary() {
  for (const BranchPointInfo &branchPoint : branchPoints) {
    std::map<unsigned, std::string> targetsAndIds;
    for (const unsigned &target : branchPoint.targetLineNumbers) {
      targetsAndIds[target] = "br_" + std::to_string(++branchCount);
    }
    branchDictionary[branchPoint.branchPoint] = targetsAndIds;
  }
}

void KeyPointsCollector::transformProgram() {

  // First, open original file for reading, and modified file for writing.
  std::ifstream originalProgram(filename);
  std::ofstream modifiedProgram(MODIFIED_PROGAM_OUT);

  // Check files opened successfully
  if (originalProgram.good() && modifiedProgram.good()) {

    // First write the header to the output file
    modifiedProgram << TRANSFORM_HEADER;

    // Keep track of line numbers
    unsigned lineNum = 1;

    // Containers for current line and new lines
    std::string currentLine;

    // Current function being analyzed
    std::shared_ptr<FunctionDeclInfo> currentFunction = nullptr;

    // Amount of branches within current function.
    int branchCountCurrFunc;

    // Get ref to function decls
    std::map<unsigned, std::shared_ptr<FunctionDeclInfo>> funcDecls =
        getFuncDecls();

    // Get ref to branch dictionary
    std::map<unsigned, std::map<unsigned, std::string>> branchDict =
        getBranchDictionary();

    // Keep track of line branch point line numbers that have been encountered.
    std::vector<unsigned> foundPoints;

    // Core iteration over original program
    while (getline(originalProgram, currentLine)) {

      // If previous line is a function def/decl, insert the branch points for
      // that function and set current function.
      if (MAP_FIND(funcDecls, lineNum - 1)) {
        currentFunction = funcDecls[lineNum - 1];
        foundPoints.clear();
        branchCountCurrFunc = 0;
        insertFunctionBranchPointDecls(modifiedProgram, currentFunction,
                                       &branchCountCurrFunc);
      }

      // If we have a current function AND the previous line is the end of said
      // function, create a pointer pointer for that function so we can access
      // it for logging purposes. Also, the current function shouldn't be main.
      if (currentFunction != nullptr &&
          (lineNum - 1) == currentFunction->endLoc &&
          currentFunction->name.compare("main")) {
        modifiedProgram << DECLARE_FUNC_PTR(currentFunction);
      }

      // If the previous line was a branch point, set the branch
      if (MAP_FIND(branchDict, lineNum - 1)) {
        modifiedProgram << SET_BRANCH(foundPoints.size());
        foundPoints.push_back(lineNum - 1);
      }

      // Iterate over found branch points and look for targets

      // This vector holds not the location of the branch point, but the INDEX
      // of the branches location in the foundPoints vector above. This is done
      // as the index is how we access BRANCH_X in the transformed program.
      std::vector<unsigned> foundPointsIdxCurrentLine;

      if (!foundPoints.empty()) {
        for (int idx = foundPoints.size() - 1; idx >= 0; --idx) {

          // Get targets for BP
          std::map<unsigned, std::string> targets =
              branchDict[foundPoints[idx]];

          // If target exists for any branch point, add to list for the current
          // line number.
          if (MAP_FIND(targets, lineNum)) {
            foundPointsIdxCurrentLine.push_back(idx);
          }
        }
      }

      // After targets are found, insert proper logging logic into modified
      // program.
      switch (foundPointsIdxCurrentLine.size()) {
        // None? Get outta there.
      case 0:
        break;
      // If only one target for the line number, check to see if all successive
      // branch points have NOT been set, this prevents unecessary logging after
      // the exit of something like an if block. e.g if the target is from
      // BRANCH_0,  ensure that BRANCH_1...BRANCH_N arent set = 1;
      case 1: {
        // If branch actually has successive points, then construct a
        // conditional.
        if (foundPointsIdxCurrentLine[0] + 1 < branchCountCurrFunc) {
          modifiedProgram << "if (";
          for (int successive = foundPointsIdxCurrentLine[0] + 1;
               successive < branchCountCurrFunc; successive++) {
            modifiedProgram << "!BRANCH_" << successive;
            if (branchCountCurrFunc - successive > 1)
              modifiedProgram << " && ";
          }
          modifiedProgram
              << ") LOG(\""
              << branchDict[foundPoints[foundPointsIdxCurrentLine[0]]][lineNum]
              << "\");";
        }
        // If not, just log it.
        else {
          modifiedProgram
              << "LOG(\""
              << branchDict[foundPoints[foundPointsIdxCurrentLine[0]]][lineNum]
              << "\");";
        }
        break;
      }
      // If two targets for the current line number, we can insert a simple if
      // else block
      case 2: {
        modifiedProgram
            << "if (BRANCH_" << foundPointsIdxCurrentLine[0] << ") {LOG(\""
            << branchDict[foundPoints[foundPointsIdxCurrentLine[0]]][lineNum]
            << "\")} else {LOG(\""
            << branchDict[foundPoints[foundPointsIdxCurrentLine[1]]][lineNum]
            << "\")}";
        break;
      }
      // Default is more than 2, in this case, we need to insert a proper if,
      // else if, else chain for all the targets available for the current line
      // number.
      default: {
        // Insert initial if block
        modifiedProgram
            << "if (BRANCH_" << foundPointsIdxCurrentLine[0] << ") {LOG(\""
            << branchDict[foundPoints[foundPointsIdxCurrentLine[0]]][lineNum]
            << "\")}";

        // Insert else if blocks for all branches before the last.
        for (int successive = 1;
             successive < foundPointsIdxCurrentLine.size() - 1; successive++) {
          modifiedProgram
              << " else if (BRANCH_" << foundPointsIdxCurrentLine[successive]
              << ") {LOG(\""
              << branchDict[foundPoints[foundPointsIdxCurrentLine[successive]]]
                           [lineNum]
              << "\")}";
        }

        // Insert final else for the last branch point.
        modifiedProgram
            << "else {LOG(\""
            << branchDict[foundPoints[foundPointsIdxCurrentLine
                                          [foundPointsIdxCurrentLine.size() -
                                           1]]][lineNum]
            << "\")}";

      } break;
      }

      // Write line
      modifiedProgram << WRITE_LINE(currentLine);
      lineNum++;
    }

    // Close files
    originalProgram.close();
    modifiedProgram.close();

  } else {
    std::cerr << "Error opening program files for transformation!\n";
    exit(EXIT_FAILURE);
  }
}

void KeyPointsCollector::insertFunctionBranchPointDecls(
    std::ofstream &program, std::shared_ptr<FunctionDeclInfo> function,
    int *branchCount) {
  // Iterate over range of function and check for branching points.
  for (int lineNum = function->defLoc; lineNum < function->endLoc; lineNum++) {
    if (MAP_FIND(getBranchDictionary(), lineNum)) {
      program << DECLARE_BRANCH((*branchCount)++);
    }
  }
  program << '\n';
}

void KeyPointsCollector::compileModified() {
  // See what compiler we are working with on the machine.
#if defined(__clang__)
  std::string c_compiler("clang");
#elif defined(__GNUC__)
  std::string c_compiler("gcc");
#endif
  if (c_compiler.empty()) {
    c_compiler = std::getenv("CC");
    if (c_compiler.empty()) {
      std::cerr << "No viable C compiler found on system!\n";
      exit(EXIT_FAILURE);
    }
  }
  std::cout << "C compiler is: " << c_compiler << '\n';

  // Ensure that the modified program exists
  if (!static_cast<bool>(std::ifstream(MODIFIED_PROGAM_OUT).good())) {
    std::cerr << "Transformed program has not been created yet!\n";
    exit(EXIT_FAILURE);
  }

  // Construct compilation command.
  std::stringstream compilationCommand;
  compilationCommand << c_compiler << " " << MODIFIED_PROGAM_OUT << " -o "
                     << EXE_OUT;

  // Compile
  bool compiled = static_cast<bool>(system(compilationCommand.str().c_str()));

  // Check if compiled properly
  if (compiled == EXIT_SUCCESS) {
    /* std::remove(EXE_OUT.c_str()); */
    std::cout << "Compilation Successful" << '\n';
  } else {
    std::cerr << "There was an error with compilation, exiting!\n";
    exit(EXIT_FAILURE);
  }
}

void KeyPointsCollector::invokeValgrind() {}

void KeyPointsCollector::executeToolchain() {
  collectCursors();
  createDictionaryFile();
  transformProgram();
  compileModified();
  std::cout << "\nToolchain was successful, the branch dicitonary, modified "
               "file, and executable have been written to the "
            << OUT_DIR << " directory \n";
}
