/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 sw=2 et tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HTMLEditor.h"

#include <algorithm>
#include <utility>

#include "AutoRangeArray.h"
#include "CSSEditUtils.h"
#include "EditAction.h"
#include "EditorDOMPoint.h"
#include "EditorUtils.h"
#include "HTMLEditHelpers.h"
#include "HTMLEditUtils.h"
#include "WSRunObject.h"

#include "js/ErrorReport.h"
#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/ComputedStyle.h"  // for ComputedStyle
#include "mozilla/ContentIterator.h"
#include "mozilla/EditorDOMPoint.h"
#include "mozilla/InternalMutationEvent.h"
#include "mozilla/Maybe.h"
#include "mozilla/OwningNonNull.h"
#include "mozilla/StaticPrefs_editor.h"  // for StaticPrefs::editor_*
#include "mozilla/Unused.h"
#include "mozilla/dom/AncestorIterator.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLBRElement.h"
#include "mozilla/dom/Selection.h"
#include "mozilla/mozalloc.h"
#include "nsAString.h"
#include "nsAtom.h"
#include "nsComputedDOMStyle.h"  // for nsComputedDOMStyle
#include "nsContentUtils.h"
#include "nsDebug.h"
#include "nsError.h"
#include "nsFrameSelection.h"
#include "nsGkAtoms.h"
#include "nsIContent.h"
#include "nsINode.h"
#include "nsRange.h"
#include "nsString.h"
#include "nsStringFwd.h"
#include "nsStyleConsts.h"  // for StyleWhiteSpace
#include "nsTArray.h"

// NOTE: This file was split from:
//   https://searchfox.org/mozilla-central/rev/c409dd9235c133ab41eba635f906aa16e050c197/editor/libeditor/HTMLEditSubActionHandler.cpp

namespace mozilla {

using namespace dom;
using EmptyCheckOption = HTMLEditUtils::EmptyCheckOption;
using InvisibleWhiteSpaces = HTMLEditUtils::InvisibleWhiteSpaces;
using LeafNodeType = HTMLEditUtils::LeafNodeType;
using ScanLineBreak = HTMLEditUtils::ScanLineBreak;
using StyleDifference = HTMLEditUtils::StyleDifference;
using TableBoundary = HTMLEditUtils::TableBoundary;
using WalkTreeOption = HTMLEditUtils::WalkTreeOption;

template nsresult HTMLEditor::DeleteTextAndTextNodesWithTransaction(
    const EditorDOMPoint& aStartPoint, const EditorDOMPoint& aEndPoint,
    TreatEmptyTextNodes aTreatEmptyTextNodes);
template nsresult HTMLEditor::DeleteTextAndTextNodesWithTransaction(
    const EditorDOMPointInText& aStartPoint,
    const EditorDOMPointInText& aEndPoint,
    TreatEmptyTextNodes aTreatEmptyTextNodes);

/*****************************************************************************
 * AutoSetTemporaryAncestorLimiter
 ****************************************************************************/

class MOZ_RAII AutoSetTemporaryAncestorLimiter final {
 public:
  AutoSetTemporaryAncestorLimiter(const HTMLEditor& aHTMLEditor,
                                  Selection& aSelection,
                                  nsINode& aStartPointNode,
                                  AutoRangeArray* aRanges = nullptr) {
    MOZ_ASSERT(aSelection.GetType() == SelectionType::eNormal);

    if (aSelection.GetAncestorLimiter()) {
      return;
    }

    Element* selectionRootElement =
        aHTMLEditor.FindSelectionRoot(aStartPointNode);
    if (!selectionRootElement) {
      return;
    }
    aHTMLEditor.InitializeSelectionAncestorLimit(*selectionRootElement);
    mSelection = &aSelection;
    // Setting ancestor limiter may change ranges which were outer of
    // the new limiter.  Therefore, we need to reinitialize aRanges.
    if (aRanges) {
      aRanges->Initialize(aSelection);
    }
  }

  ~AutoSetTemporaryAncestorLimiter() {
    if (mSelection) {
      mSelection->SetAncestorLimiter(nullptr);
    }
  }

 private:
  RefPtr<Selection> mSelection;
};

/*****************************************************************************
 * AutoDeleteRangesHandler
 ****************************************************************************/

class MOZ_STACK_CLASS HTMLEditor::AutoDeleteRangesHandler final {
 public:
  explicit AutoDeleteRangesHandler(
      const AutoDeleteRangesHandler* aParent = nullptr)
      : mParent(aParent),
        mOriginalDirectionAndAmount(nsIEditor::eNone),
        mOriginalStripWrappers(nsIEditor::eNoStrip) {}

  /**
   * ComputeRangesToDelete() computes actual deletion ranges.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult ComputeRangesToDelete(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete, const Element& aEditingHost);

  /**
   * Deletes content in or around aRangesToDelete.
   * NOTE: This method creates SelectionBatcher.  Therefore, each caller
   *       needs to check if the editor is still available even if this returns
   *       NS_OK.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult> Run(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
      const Element& aEditingHost);

 private:
  bool IsHandlingRecursively() const { return mParent != nullptr; }

  bool CanFallbackToDeleteRangesWithTransaction(
      const AutoRangeArray& aRangesToDelete) const {
    return !IsHandlingRecursively() && !aRangesToDelete.Ranges().IsEmpty() &&
           (!aRangesToDelete.IsCollapsed() ||
            EditorBase::HowToHandleCollapsedRangeFor(
                mOriginalDirectionAndAmount) !=
                EditorBase::HowToHandleCollapsedRange::Ignore);
  }

  /**
   * HandleDeleteAroundCollapsedRanges() handles deletion with collapsed
   * ranges.  Callers must guarantee that this is called only when
   * aRangesToDelete.IsCollapsed() returns true.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aStripWrappers      Must be eStrip or eNoStrip.
   * @param aRangesToDelete     Ranges to delete.  This `IsCollapsed()` must
   *                            return true.
   * @param aWSRunScannerAtCaret        Scanner instance which scanned from
   *                                    caret point.
   * @param aScanFromCaretPointResult   Scan result of aWSRunScannerAtCaret
   *                                    toward aDirectionAndAmount.
   * @param aEditingHost        The editing host.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteAroundCollapsedRanges(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
      const WSRunScanner& aWSRunScannerAtCaret,
      const WSScanResult& aScanFromCaretPointResult,
      const Element& aEditingHost);
  nsresult ComputeRangesToDeleteAroundCollapsedRanges(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete, const WSRunScanner& aWSRunScannerAtCaret,
      const WSScanResult& aScanFromCaretPointResult,
      const Element& aEditingHost) const;

  /**
   * HandleDeleteNonCollapsedRanges() handles deletion with non-collapsed
   * ranges.  Callers must guarantee that this is called only when
   * aRangesToDelete.IsCollapsed() returns false.
   *
   * @param aDirectionAndAmount         Direction of the deletion.
   * @param aStripWrappers              Must be eStrip or eNoStrip.
   * @param aRangesToDelete             The ranges to delete.
   * @param aSelectionWasCollapsed      If the caller extended `Selection`
   *                                    from collapsed, set this to `Yes`.
   *                                    Otherwise, i.e., `Selection` is not
   *                                    collapsed from the beginning, set
   *                                    this to `No`.
   * @param aEditingHost                The editing host.
   */
  enum class SelectionWasCollapsed { Yes, No };
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteNonCollapsedRanges(HTMLEditor& aHTMLEditor,
                                 nsIEditor::EDirection aDirectionAndAmount,
                                 nsIEditor::EStripWrappers aStripWrappers,
                                 AutoRangeArray& aRangesToDelete,
                                 SelectionWasCollapsed aSelectionWasCollapsed,
                                 const Element& aEditingHost);
  nsresult ComputeRangesToDeleteNonCollapsedRanges(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete,
      SelectionWasCollapsed aSelectionWasCollapsed,
      const Element& aEditingHost) const;

  /**
   * HandleDeleteTextAroundCollapsedRanges() handles deletion of collapsed
   * ranges in a text node.
   *
   * @param aDirectionAndAmount Must be eNext or ePrevious.
   * @param aCaretPoisition     The position where caret is.  This container
   *                            must be a text node.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteTextAroundCollapsedRanges(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete);
  nsresult ComputeRangesToDeleteTextAroundCollapsedRanges(
      Element* aEditingHost, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete) const;

  /**
   * HandleDeleteCollapsedSelectionAtWhiteSpaces() handles deletion of
   * collapsed selection at white-spaces in a text node.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aPointToDelete      The point to delete.  I.e., typically, caret
   *                            position.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteCollapsedSelectionAtWhiteSpaces(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      const EditorDOMPoint& aPointToDelete);

  /**
   * HandleDeleteCollapsedSelectionAtVisibleChar() handles deletion of
   * collapsed selection in a text node.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aPointToDelete      The point in a text node to delete character(s).
   *                            Caller must guarantee that this is in a text
   *                            node.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteCollapsedSelectionAtVisibleChar(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      const EditorDOMPoint& aPointToDelete);

  /**
   * HandleDeleteAtomicContent() handles deletion of atomic elements like
   * `<br>`, `<hr>`, `<img>`, `<input>`, etc and data nodes except text node
   * (e.g., comment node). Note that don't call this directly with `<hr>`
   * element.  Instead, call `HandleDeleteHRElement()`. Note that don't call
   * this for invisible `<br>` element.
   *
   * @param aAtomicContent      The atomic content to be deleted.
   * @param aCaretPoint         The caret point (i.e., selection start or
   *                            end).
   * @param aWSRunScannerAtCaret WSRunScanner instance which was initialized
   *                             with the caret point.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteAtomicContent(HTMLEditor& aHTMLEditor, nsIContent& aAtomicContent,
                            const EditorDOMPoint& aCaretPoint,
                            const WSRunScanner& aWSRunScannerAtCaret);
  nsresult ComputeRangesToDeleteAtomicContent(
      Element* aEditingHost, const nsIContent& aAtomicContent,
      AutoRangeArray& aRangesToDelete) const;

  /**
   * GetAtomicContnetToDelete() returns better content that is deletion of
   * atomic element.  If aScanFromCaretPointResult is special, since this
   * point may not be editable, we look for better point to remove atomic
   * content.
   *
   * @param aDirectionAndAmount       Direction of the deletion.
   * @param aWSRunScannerAtCaret      WSRunScanner instance which was
   *                                  initialized with the caret point.
   * @param aScanFromCaretPointResult Scan result of aWSRunScannerAtCaret
   *                                  toward aDirectionAndAmount.
   */
  static nsIContent* GetAtomicContentToDelete(
      nsIEditor::EDirection aDirectionAndAmount,
      const WSRunScanner& aWSRunScannerAtCaret,
      const WSScanResult& aScanFromCaretPointResult) MOZ_NONNULL_RETURN;

  /**
   * HandleDeleteHRElement() handles deletion around `<hr>` element.  If
   * aDirectionAndAmount is nsIEditor::ePrevious, aHTElement is removed only
   * when caret is at next sibling of the `<hr>` element and inter line position
   * is "left".  Otherwise, caret is moved and does not remove the `<hr>`
   * elemnent.
   * XXX Perhaps, we can get rid of this special handling because the other
   *     browsers don't do this, and our `<hr>` element handling is really
   *     odd.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aHRElement          The `<hr>` element to be removed.
   * @param aCaretPoint         The caret point (i.e., selection start or
   *                            end).
   * @param aWSRunScannerAtCaret WSRunScanner instance which was initialized
   *                             with the caret point.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteHRElement(HTMLEditor& aHTMLEditor,
                        nsIEditor::EDirection aDirectionAndAmount,
                        Element& aHRElement, const EditorDOMPoint& aCaretPoint,
                        const WSRunScanner& aWSRunScannerAtCaret);
  nsresult ComputeRangesToDeleteHRElement(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      Element& aHRElement, const EditorDOMPoint& aCaretPoint,
      const WSRunScanner& aWSRunScannerAtCaret,
      AutoRangeArray& aRangesToDelete) const;

  /**
   * HandleDeleteAtOtherBlockBoundary() handles deletion at other block boundary
   * (i.e., immediately before or after a block). If this does not join blocks,
   * `Run()` may be called recursively with creating another instance.
   *
   * @param aDirectionAndAmount Direction of the deletion.
   * @param aStripWrappers      Must be eStrip or eNoStrip.
   * @param aOtherBlockElement  The block element which follows the caret or
   *                            is followed by caret.
   * @param aCaretPoint         The caret point (i.e., selection start or
   *                            end).
   * @param aWSRunScannerAtCaret WSRunScanner instance which was initialized
   *                             with the caret point.
   * @param aRangesToDelete     Ranges to delete of the caller.  This should
   *                            be collapsed and the point should match with
   *                            aCaretPoint.
   * @param aEditingHost        The editing host.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  HandleDeleteAtOtherBlockBoundary(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      nsIEditor::EStripWrappers aStripWrappers, Element& aOtherBlockElement,
      const EditorDOMPoint& aCaretPoint, WSRunScanner& aWSRunScannerAtCaret,
      AutoRangeArray& aRangesToDelete, const Element& aEditingHost);

  /**
   * ExtendOrShrinkRangeToDelete() extends aRangeToDelete if there are
   * an invisible <br> element and/or some parent empty elements.
   *
   * @param aFrameSelection     If the caller wants range in selection limiter,
   *                            set this to non-nullptr which knows the limiter.
   * @param aRangeToDelete       The range to be extended for deletion.  This
   *                            must not be collapsed, must be positioned.
   */
  template <typename EditorDOMRangeType>
  Result<EditorRawDOMRange, nsresult> ExtendOrShrinkRangeToDelete(
      const HTMLEditor& aHTMLEditor, const nsFrameSelection* aFrameSelection,
      const EditorDOMRangeType& aRangeToDelete) const;

  /**
   * ShouldDeleteHRElement() checks whether aHRElement should be deleted
   * when selection is collapsed at aCaretPoint.
   */
  Result<bool, nsresult> ShouldDeleteHRElement(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      Element& aHRElement, const EditorDOMPoint& aCaretPoint) const;

  /**
   * DeleteUnnecessaryNodesAndCollapseSelection() removes unnecessary nodes
   * around aSelectionStartPoint and aSelectionEndPoint.  Then, collapse
   * selection at aSelectionStartPoint or aSelectionEndPoint (depending on
   * aDirectionAndAmount).
   *
   * @param aDirectionAndAmount         Direction of the deletion.
   *                                    If nsIEditor::ePrevious, selection
   * will be collapsed to aSelectionEndPoint. Otherwise, selection will be
   * collapsed to aSelectionStartPoint.
   * @param aSelectionStartPoint        First selection range start after
   *                                    computing the deleting range.
   * @param aSelectionEndPoint          First selection range end after
   *                                    computing the deleting range.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
  DeleteUnnecessaryNodesAndCollapseSelection(
      HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      const EditorDOMPoint& aSelectionStartPoint,
      const EditorDOMPoint& aSelectionEndPoint);

  /**
   * If aContent is a text node that contains only collapsed white-space or
   * empty and editable.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
  DeleteNodeIfInvisibleAndEditableTextNode(HTMLEditor& aHTMLEditor,
                                           nsIContent& aContent);

  /**
   * DeleteParentBlocksIfEmpty() removes parent block elements if they
   * don't have visible contents.  Note that due performance issue of
   * WhiteSpaceVisibilityKeeper, this call may be expensive.  And also note that
   * this removes a empty block with a transaction.  So, please make sure that
   * you've already created `AutoPlaceholderBatch`.
   *
   * @param aPoint      The point whether this method climbing up the DOM
   *                    tree to remove empty parent blocks.
   * @return            NS_OK if one or more empty block parents are deleted.
   *                    NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND if the point is
   *                    not in empty block.
   *                    Or NS_ERROR_* if something unexpected occurs.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
  DeleteParentBlocksWithTransactionIfEmpty(HTMLEditor& aHTMLEditor,
                                           const EditorDOMPoint& aPoint);

  [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
  FallbackToDeleteRangesWithTransaction(HTMLEditor& aHTMLEditor,
                                        AutoRangeArray& aRangesToDelete) const {
    MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
    MOZ_ASSERT(CanFallbackToDeleteRangesWithTransaction(aRangesToDelete));
    nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
        mOriginalDirectionAndAmount, mOriginalStripWrappers, aRangesToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteRangesWithTransaction() failed");
      return Err(rv);
    }
    // Don't return "ignored" for avoiding to fall it back again.
    return EditActionResult::HandledResult();
  }

  /**
   * ComputeRangesToDeleteRangesWithTransaction() computes target ranges
   * which will be called by `EditorBase::DeleteRangesWithTransaction()`.
   * TODO: We should not use it for consistency with each deletion handler
   *       in this and nested classes.
   */
  nsresult ComputeRangesToDeleteRangesWithTransaction(
      const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
      AutoRangeArray& aRangesToDelete) const;

  nsresult FallbackToComputeRangesToDeleteRangesWithTransaction(
      const HTMLEditor& aHTMLEditor, AutoRangeArray& aRangesToDelete) const {
    MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
    MOZ_ASSERT(CanFallbackToDeleteRangesWithTransaction(aRangesToDelete));
    nsresult rv = ComputeRangesToDeleteRangesWithTransaction(
        aHTMLEditor, mOriginalDirectionAndAmount, aRangesToDelete);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoDeleteRangesHandler::"
                         "ComputeRangesToDeleteRangesWithTransaction() failed");
    return rv;
  }

  class MOZ_STACK_CLASS AutoBlockElementsJoiner final {
   public:
    AutoBlockElementsJoiner() = delete;
    explicit AutoBlockElementsJoiner(
        AutoDeleteRangesHandler& aDeleteRangesHandler)
        : mDeleteRangesHandler(&aDeleteRangesHandler),
          mDeleteRangesHandlerConst(aDeleteRangesHandler) {}
    explicit AutoBlockElementsJoiner(
        const AutoDeleteRangesHandler& aDeleteRangesHandler)
        : mDeleteRangesHandler(nullptr),
          mDeleteRangesHandlerConst(aDeleteRangesHandler) {}

    /**
     * PrepareToDeleteAtCurrentBlockBoundary() considers left content and right
     * content which are joined for handling deletion at current block boundary
     * (i.e., at start or end of the current block).
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aCurrentBlockElement      The current block element.
     * @param aCaretPoint               The caret point (i.e., selection start
     *                                  or end).
     * @return                          true if can continue to handle the
     *                                  deletion.
     */
    bool PrepareToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        Element& aCurrentBlockElement, const EditorDOMPoint& aCaretPoint);

    /**
     * PrepareToDeleteAtOtherBlockBoundary() considers left content and right
     * content which are joined for handling deletion at other block boundary
     * (i.e., immediately before or after a block).
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aOtherBlockElement        The block element which follows the
     *                                  caret or is followed by caret.
     * @param aCaretPoint               The caret point (i.e., selection start
     *                                  or end).
     * @param aWSRunScannerAtCaret      WSRunScanner instance which was
     *                                  initialized with the caret point.
     * @return                          true if can continue to handle the
     *                                  deletion.
     */
    bool PrepareToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount, Element& aOtherBlockElement,
        const EditorDOMPoint& aCaretPoint,
        const WSRunScanner& aWSRunScannerAtCaret);

    /**
     * PrepareToDeleteNonCollapsedRanges() considers left block element and
     * right block element which are inclusive ancestor block element of
     * start and end container of first range of aRangesToDelete.
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aRangesToDelete           Ranges to delete.  Must not be
     *                                  collapsed.
     * @return                          true if can continue to handle the
     *                                  deletion.
     */
    bool PrepareToDeleteNonCollapsedRanges(
        const HTMLEditor& aHTMLEditor, const AutoRangeArray& aRangesToDelete);

    /**
     * Run() executes the joining.
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aStripWrappers            Must be eStrip or eNoStrip.
     * @param aCaretPoint               The caret point (i.e., selection start
     *                                  or end).
     * @param aRangesToDelete           Ranges to delete of the caller.
     *                                  This should be collapsed and match
     *                                  with aCaretPoint.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult> Run(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        const EditorDOMPoint& aCaretPoint, AutoRangeArray& aRangesToDelete,
        const Element& aEditingHost) {
      switch (mMode) {
        case Mode::JoinCurrentBlock: {
          Result<EditActionResult, nsresult> result =
              HandleDeleteAtCurrentBlockBoundary(aHTMLEditor, aCaretPoint,
                                                 aEditingHost);
          NS_WARNING_ASSERTION(result.isOk(),
                               "AutoBlockElementsJoiner::"
                               "HandleDeleteAtCurrentBlockBoundary() failed");
          return result;
        }
        case Mode::JoinOtherBlock: {
          Result<EditActionResult, nsresult> result =
              HandleDeleteAtOtherBlockBoundary(aHTMLEditor, aDirectionAndAmount,
                                               aStripWrappers, aCaretPoint,
                                               aRangesToDelete, aEditingHost);
          NS_WARNING_ASSERTION(result.isOk(),
                               "AutoBlockElementsJoiner::"
                               "HandleDeleteAtOtherBlockBoundary() failed");
          return result;
        }
        case Mode::DeleteBRElement: {
          Result<EditActionResult, nsresult> result =
              DeleteBRElement(aHTMLEditor, aDirectionAndAmount, aCaretPoint);
          NS_WARNING_ASSERTION(
              result.isOk(),
              "AutoBlockElementsJoiner::DeleteBRElement() failed");
          return result;
        }
        case Mode::JoinBlocksInSameParent:
        case Mode::DeleteContentInRanges:
        case Mode::DeleteNonCollapsedRanges:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other Run()");
          return Err(NS_ERROR_UNEXPECTED);
        case Mode::NotInitialized:
          return EditActionResult::IgnoredResult();
      }
      return Err(NS_ERROR_NOT_INITIALIZED);
    }

    nsresult ComputeRangesToDelete(const HTMLEditor& aHTMLEditor,
                                   nsIEditor::EDirection aDirectionAndAmount,
                                   const EditorDOMPoint& aCaretPoint,
                                   AutoRangeArray& aRangesToDelete,
                                   const Element& aEditingHost) const {
      switch (mMode) {
        case Mode::JoinCurrentBlock: {
          nsresult rv = ComputeRangesToDeleteAtCurrentBlockBoundary(
              aHTMLEditor, aCaretPoint, aRangesToDelete, aEditingHost);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToDeleteAtCurrentBlockBoundary() failed");
          return rv;
        }
        case Mode::JoinOtherBlock: {
          nsresult rv = ComputeRangesToDeleteAtOtherBlockBoundary(
              aHTMLEditor, aDirectionAndAmount, aCaretPoint, aRangesToDelete,
              aEditingHost);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToDeleteAtOtherBlockBoundary() failed");
          return rv;
        }
        case Mode::DeleteBRElement: {
          nsresult rv = ComputeRangesToDeleteBRElement(aRangesToDelete);
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                               "AutoBlockElementsJoiner::"
                               "ComputeRangesToDeleteBRElement() failed");
          return rv;
        }
        case Mode::JoinBlocksInSameParent:
        case Mode::DeleteContentInRanges:
        case Mode::DeleteNonCollapsedRanges:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other "
              "ComputeRangesToDelete()");
          return NS_ERROR_UNEXPECTED;
        case Mode::NotInitialized:
          return NS_OK;
      }
      return NS_ERROR_NOT_IMPLEMENTED;
    }

    /**
     * Run() executes the joining.
     *
     * @param aHTMLEditor               The HTML editor.
     * @param aDirectionAndAmount       Direction of the deletion.
     * @param aStripWrappers            Whether delete or keep new empty
     *                                  ancestor elements.
     * @param aRangesToDelete           Ranges to delete.  Must not be
     *                                  collapsed.
     * @param aSelectionWasCollapsed    Whether selection was or was not
     *                                  collapsed when starting to handle
     *                                  deletion.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult> Run(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed,
        const Element& aEditingHost) {
      switch (mMode) {
        case Mode::JoinCurrentBlock:
        case Mode::JoinOtherBlock:
        case Mode::DeleteBRElement:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other Run()");
          return Err(NS_ERROR_UNEXPECTED);
        case Mode::JoinBlocksInSameParent: {
          Result<EditActionResult, nsresult> result =
              JoinBlockElementsInSameParent(aHTMLEditor, aDirectionAndAmount,
                                            aStripWrappers, aRangesToDelete);
          NS_WARNING_ASSERTION(result.isOk(),
                               "AutoBlockElementsJoiner::"
                               "JoinBlockElementsInSameParent() failed");
          return result;
        }
        case Mode::DeleteContentInRanges: {
          Result<EditActionResult, nsresult> result =
              DeleteContentInRanges(aHTMLEditor, aDirectionAndAmount,
                                    aStripWrappers, aRangesToDelete);
          NS_WARNING_ASSERTION(
              result.isOk(),
              "AutoBlockElementsJoiner::DeleteContentInRanges() failed");
          return result;
        }
        case Mode::DeleteNonCollapsedRanges: {
          Result<EditActionResult, nsresult> result =
              HandleDeleteNonCollapsedRanges(
                  aHTMLEditor, aDirectionAndAmount, aStripWrappers,
                  aRangesToDelete, aSelectionWasCollapsed, aEditingHost);
          NS_WARNING_ASSERTION(result.isOk(),
                               "AutoBlockElementsJoiner::"
                               "HandleDeleteNonCollapsedRange() failed");
          return result;
        }
        case Mode::NotInitialized:
          MOZ_ASSERT_UNREACHABLE(
              "Call Run() after calling a preparation method");
          return EditActionResult::IgnoredResult();
      }
      return Err(NS_ERROR_NOT_INITIALIZED);
    }

    nsresult ComputeRangesToDelete(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed,
        const Element& aEditingHost) const {
      switch (mMode) {
        case Mode::JoinCurrentBlock:
        case Mode::JoinOtherBlock:
        case Mode::DeleteBRElement:
          MOZ_ASSERT_UNREACHABLE(
              "This mode should be handled in the other "
              "ComputeRangesToDelete()");
          return NS_ERROR_UNEXPECTED;
        case Mode::JoinBlocksInSameParent: {
          nsresult rv = ComputeRangesToJoinBlockElementsInSameParent(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToJoinBlockElementsInSameParent() failed");
          return rv;
        }
        case Mode::DeleteContentInRanges: {
          nsresult rv = ComputeRangesToDeleteContentInRanges(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                               "AutoBlockElementsJoiner::"
                               "ComputeRangesToDeleteContentInRanges() failed");
          return rv;
        }
        case Mode::DeleteNonCollapsedRanges: {
          nsresult rv = ComputeRangesToDeleteNonCollapsedRanges(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete,
              aSelectionWasCollapsed, aEditingHost);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "AutoBlockElementsJoiner::"
              "ComputeRangesToDeleteNonCollapsedRanges() failed");
          return rv;
        }
        case Mode::NotInitialized:
          MOZ_ASSERT_UNREACHABLE(
              "Call ComputeRangesToDelete() after calling a preparation "
              "method");
          return NS_ERROR_NOT_INITIALIZED;
      }
      return NS_ERROR_NOT_INITIALIZED;
    }

    nsIContent* GetLeafContentInOtherBlockElement() const {
      MOZ_ASSERT(mMode == Mode::JoinOtherBlock);
      return mLeafContentInOtherBlock;
    }

   private:
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
    HandleDeleteAtCurrentBlockBoundary(HTMLEditor& aHTMLEditor,
                                       const EditorDOMPoint& aCaretPoint,
                                       const Element& aEditingHost);
    nsresult ComputeRangesToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor, const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete, const Element& aEditingHost) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
    HandleDeleteAtOtherBlockBoundary(HTMLEditor& aHTMLEditor,
                                     nsIEditor::EDirection aDirectionAndAmount,
                                     nsIEditor::EStripWrappers aStripWrappers,
                                     const EditorDOMPoint& aCaretPoint,
                                     AutoRangeArray& aRangesToDelete,
                                     const Element& aEditingHost);
    // FYI: This method may modify selection, but it won't cause running
    //      script because of `AutoHideSelectionChanges` which blocks
    //      selection change listeners and the selection change event
    //      dispatcher.
    MOZ_CAN_RUN_SCRIPT_BOUNDARY nsresult
    ComputeRangesToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aCaretPoint, AutoRangeArray& aRangesToDelete,
        const Element& aEditingHost) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
    JoinBlockElementsInSameParent(HTMLEditor& aHTMLEditor,
                                  nsIEditor::EDirection aDirectionAndAmount,
                                  nsIEditor::EStripWrappers aStripWrappers,
                                  AutoRangeArray& aRangesToDelete);
    nsresult ComputeRangesToJoinBlockElementsInSameParent(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
    DeleteBRElement(HTMLEditor& aHTMLEditor,
                    nsIEditor::EDirection aDirectionAndAmount,
                    const EditorDOMPoint& aCaretPoint);
    nsresult ComputeRangesToDeleteBRElement(
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
    DeleteContentInRanges(HTMLEditor& aHTMLEditor,
                          nsIEditor::EDirection aDirectionAndAmount,
                          nsIEditor::EStripWrappers aStripWrappers,
                          AutoRangeArray& aRangesToDelete);
    nsresult ComputeRangesToDeleteContentInRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const;
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult>
    HandleDeleteNonCollapsedRanges(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed,
        const Element& aEditingHost);
    nsresult ComputeRangesToDeleteNonCollapsedRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed,
        const Element& aEditingHost) const;

    /**
     * JoinNodesDeepWithTransaction() joins aLeftNode and aRightNode "deeply".
     * First, they are joined simply, then, new right node is assumed as the
     * child at length of the left node before joined and new left node is
     * assumed as its previous sibling.  Then, they will be joined again.
     * And then, these steps are repeated.
     *
     * @param aLeftContent    The node which will be removed form the tree.
     * @param aRightContent   The node which will be inserted the contents of
     *                        aRightContent.
     * @return                The point of the first child of the last right
     * node. The result is always set if this succeeded.
     */
    MOZ_CAN_RUN_SCRIPT Result<EditorDOMPoint, nsresult>
    JoinNodesDeepWithTransaction(HTMLEditor& aHTMLEditor,
                                 nsIContent& aLeftContent,
                                 nsIContent& aRightContent);

    /**
     * DeleteNodesEntirelyInRangeButKeepTableStructure() removes nodes which are
     * entirely in aRange.  Howevers, if some nodes are part of a table,
     * removes all children of them instead.  I.e., this does not make damage to
     * table structure at the range, but may remove table entirely if it's
     * in the range.
     *
     * @return                  true if inclusive ancestor block elements at
     *                          start and end of the range should be joined.
     */
    MOZ_CAN_RUN_SCRIPT Result<bool, nsresult>
    DeleteNodesEntirelyInRangeButKeepTableStructure(
        HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed);
    bool NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor,
        const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const;
    Result<bool, nsresult>
    ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const;

    /**
     * DeleteContentButKeepTableStructure() removes aContent if it's an element
     * which is part of a table structure.  If it's a part of table structure,
     * removes its all children recursively.  I.e., this may delete all of a
     * table, but won't break table structure partially.
     *
     * @param aContent            The content which or whose all children should
     *                            be removed.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
    DeleteContentButKeepTableStructure(HTMLEditor& aHTMLEditor,
                                       nsIContent& aContent);

    /**
     * DeleteTextAtStartAndEndOfRange() removes text if start and/or end of
     * aRange is in a text node.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult
    DeleteTextAtStartAndEndOfRange(HTMLEditor& aHTMLEditor, nsRange& aRange);

    class MOZ_STACK_CLASS AutoInclusiveAncestorBlockElementsJoiner final {
     public:
      AutoInclusiveAncestorBlockElementsJoiner() = delete;
      AutoInclusiveAncestorBlockElementsJoiner(
          nsIContent& aInclusiveDescendantOfLeftBlockElement,
          nsIContent& aInclusiveDescendantOfRightBlockElement)
          : mInclusiveDescendantOfLeftBlockElement(
                aInclusiveDescendantOfLeftBlockElement),
            mInclusiveDescendantOfRightBlockElement(
                aInclusiveDescendantOfRightBlockElement),
            mCanJoinBlocks(false),
            mFallbackToDeleteLeafContent(false) {}

      bool IsSet() const { return mLeftBlockElement && mRightBlockElement; }
      bool IsSameBlockElement() const {
        return mLeftBlockElement && mLeftBlockElement == mRightBlockElement;
      }

      /**
       * Prepare for joining inclusive ancestor block elements.  When this
       * returns false, the deletion should be canceled.
       */
      Result<bool, nsresult> Prepare(const HTMLEditor& aHTMLEditor,
                                     const Element& aEditingHost);

      /**
       * When this returns true, this can join the blocks with `Run()`.
       */
      bool CanJoinBlocks() const { return mCanJoinBlocks; }

      /**
       * When this returns true, `Run()` must return "ignored" so that
       * caller can skip calling `Run()`.  This is available only when
       * `CanJoinBlocks()` returns `true`.
       * TODO: This should be merged into `CanJoinBlocks()` in the future.
       */
      bool ShouldDeleteLeafContentInstead() const {
        MOZ_ASSERT(CanJoinBlocks());
        return mFallbackToDeleteLeafContent;
      }

      /**
       * ComputeRangesToDelete() extends aRangesToDelete includes the element
       * boundaries between joining blocks.  If they won't be joined, this
       * collapses the range to aCaretPoint.
       */
      nsresult ComputeRangesToDelete(const HTMLEditor& aHTMLEditor,
                                     const EditorDOMPoint& aCaretPoint,
                                     AutoRangeArray& aRangesToDelete) const;

      /**
       * Join inclusive ancestor block elements which are found by preceding
       * Preare() call.
       * The right element is always joined to the left element.
       * If the elements are the same type and not nested within each other,
       * JoinEditableNodesWithTransaction() is called (example, joining two
       * list items together into one).
       * If the elements are not the same type, or one is a descendant of the
       * other, we instead destroy the right block placing its children into
       * left block.
       */
      [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult> Run(
          HTMLEditor& aHTMLEditor, const Element& aEditingHost);

     private:
      /**
       * This method returns true when
       * `MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement()`,
       * `MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement()` and
       * `MergeFirstLineOfRightBlockElementIntoLeftBlockElement()` handle it
       * with the `if` block of their main blocks.
       */
      bool CanMergeLeftAndRightBlockElements() const {
        if (!IsSet()) {
          return false;
        }
        // `MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement()`
        if (mPointContainingTheOtherBlockElement.GetContainer() ==
            mRightBlockElement) {
          return mNewListElementTagNameOfRightListElement.isSome();
        }
        // `MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement()`
        if (mPointContainingTheOtherBlockElement.GetContainer() ==
            mLeftBlockElement) {
          return mNewListElementTagNameOfRightListElement.isSome() &&
                 !mRightBlockElement->GetChildCount();
        }
        MOZ_ASSERT(!mPointContainingTheOtherBlockElement.IsSet());
        // `MergeFirstLineOfRightBlockElementIntoLeftBlockElement()`
        return mNewListElementTagNameOfRightListElement.isSome() ||
               mLeftBlockElement->NodeInfo()->NameAtom() ==
                   mRightBlockElement->NodeInfo()->NameAtom();
      }

      OwningNonNull<nsIContent> mInclusiveDescendantOfLeftBlockElement;
      OwningNonNull<nsIContent> mInclusiveDescendantOfRightBlockElement;
      RefPtr<Element> mLeftBlockElement;
      RefPtr<Element> mRightBlockElement;
      Maybe<nsAtom*> mNewListElementTagNameOfRightListElement;
      EditorDOMPoint mPointContainingTheOtherBlockElement;
      RefPtr<dom::HTMLBRElement> mPrecedingInvisibleBRElement;
      bool mCanJoinBlocks;
      bool mFallbackToDeleteLeafContent;
    };  // HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
        // AutoInclusiveAncestorBlockElementsJoiner

    enum class Mode {
      NotInitialized,
      JoinCurrentBlock,
      JoinOtherBlock,
      JoinBlocksInSameParent,
      DeleteBRElement,
      DeleteContentInRanges,
      DeleteNonCollapsedRanges,
    };
    AutoDeleteRangesHandler* mDeleteRangesHandler;
    const AutoDeleteRangesHandler& mDeleteRangesHandlerConst;
    nsCOMPtr<nsIContent> mLeftContent;
    nsCOMPtr<nsIContent> mRightContent;
    nsCOMPtr<nsIContent> mLeafContentInOtherBlock;
    // mSkippedInvisibleContents stores all content nodes which are skipped at
    // scanning mLeftContent and mRightContent.  The content nodes should be
    // removed at deletion.
    AutoTArray<OwningNonNull<nsIContent>, 8> mSkippedInvisibleContents;
    RefPtr<dom::HTMLBRElement> mBRElement;
    Mode mMode = Mode::NotInitialized;
  };  // HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner

  class MOZ_STACK_CLASS AutoEmptyBlockAncestorDeleter final {
   public:
    /**
     * ScanEmptyBlockInclusiveAncestor() scans an inclusive ancestor element
     * which is empty and a block element.  Then, stores the result and
     * returns the found empty block element.
     *
     * @param aHTMLEditor         The HTMLEditor.
     * @param aStartContent       Start content to look for empty ancestors.
     */
    [[nodiscard]] Element* ScanEmptyBlockInclusiveAncestor(
        const HTMLEditor& aHTMLEditor, nsIContent& aStartContent);

    /**
     * ComputeTargetRanges() computes "target ranges" for deleting
     * `mEmptyInclusiveAncestorBlockElement`.
     */
    nsresult ComputeTargetRanges(const HTMLEditor& aHTMLEditor,
                                 nsIEditor::EDirection aDirectionAndAmount,
                                 const Element& aEditingHost,
                                 AutoRangeArray& aRangesToDelete) const;

    /**
     * Deletes found empty block element by `ScanEmptyBlockInclusiveAncestor()`.
     * If found one is a list item element, calls
     * `MaybeInsertBRElementBeforeEmptyListItemElement()` before deleting
     * the list item element.
     * If found empty ancestor is not a list item element,
     * `GetNewCaretPoisition()` will be called to determine new caret position.
     * Finally, removes the empty block ancestor.
     *
     * @param aHTMLEditor         The HTMLEditor.
     * @param aDirectionAndAmount If found empty ancestor block is a list item
     *                            element, this is ignored.  Otherwise:
     *                            - If eNext, eNextWord or eToEndOfLine,
     *                              collapse Selection to after found empty
     *                              ancestor.
     *                            - If ePrevious, ePreviousWord or
     *                              eToBeginningOfLine, collapse Selection to
     *                              end of previous editable node.
     *                            - Otherwise, eNone is allowed but does
     *                              nothing.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<EditActionResult, nsresult> Run(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount);

   private:
    /**
     * MaybeInsertBRElementBeforeEmptyListItemElement() inserts a `<br>` element
     * if `mEmptyInclusiveAncestorBlockElement` is a list item element which
     * is first editable element in its parent, and its grand parent is not a
     * list element, inserts a `<br>` element before the empty list item.
     */
    [[nodiscard]] MOZ_CAN_RUN_SCRIPT Result<RefPtr<Element>, nsresult>
    MaybeInsertBRElementBeforeEmptyListItemElement(HTMLEditor& aHTMLEditor);

    /**
     * GetNewCaretPoisition() returns new caret position after deleting
     * `mEmptyInclusiveAncestorBlockElement`.
     */
    [[nodiscard]] Result<EditorDOMPoint, nsresult> GetNewCaretPoisition(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount) const;

    RefPtr<Element> mEmptyInclusiveAncestorBlockElement;
  };  // HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter

  const AutoDeleteRangesHandler* const mParent;
  nsIEditor::EDirection mOriginalDirectionAndAmount;
  nsIEditor::EStripWrappers mOriginalStripWrappers;
};  // HTMLEditor::AutoDeleteRangesHandler

nsresult HTMLEditor::ComputeTargetRanges(
    nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(IsEditActionDataAvailable());

  Element* editingHost = ComputeEditingHost();
  if (!editingHost) {
    aRangesToDelete.RemoveAllRanges();
    return NS_ERROR_EDITOR_NO_EDITABLE_RANGE;
  }

  // First check for table selection mode.  If so, hand off to table editor.
  SelectedTableCellScanner scanner(aRangesToDelete);
  if (scanner.IsInTableCellSelectionMode()) {
    // If it's in table cell selection mode, we'll delete all childen in
    // the all selected table cell elements,
    if (scanner.ElementsRef().Length() == aRangesToDelete.Ranges().Length()) {
      return NS_OK;
    }
    // but will ignore all ranges which does not select a table cell.
    size_t removedRanges = 0;
    for (size_t i = 1; i < scanner.ElementsRef().Length(); i++) {
      if (HTMLEditUtils::GetTableCellElementIfOnlyOneSelected(
              aRangesToDelete.Ranges()[i - removedRanges]) !=
          scanner.ElementsRef()[i]) {
        // XXX Need to manage anchor-focus range too!
        aRangesToDelete.Ranges().RemoveElementAt(i - removedRanges);
        removedRanges++;
      }
    }
    return NS_OK;
  }

  aRangesToDelete.EnsureOnlyEditableRanges(*editingHost);
  if (aRangesToDelete.Ranges().IsEmpty()) {
    NS_WARNING(
        "There is no range which we can delete entire of or around the caret");
    return NS_ERROR_EDITOR_NO_EDITABLE_RANGE;
  }
  AutoDeleteRangesHandler deleteHandler;
  // Should we delete target ranges which cannot delete actually?
  nsresult rv = deleteHandler.ComputeRangesToDelete(
      *this, aDirectionAndAmount, aRangesToDelete, *editingHost);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoDeleteRangesHandler::ComputeRangesToDelete() failed");
  return rv;
}

Result<EditActionResult, nsresult> HTMLEditor::HandleDeleteSelection(
    nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aStripWrappers == nsIEditor::eStrip ||
             aStripWrappers == nsIEditor::eNoStrip);

  if (MOZ_UNLIKELY(!SelectionRef().RangeCount())) {
    return Err(NS_ERROR_EDITOR_NO_EDITABLE_RANGE);
  }

  RefPtr<Element> editingHost = ComputeEditingHost();
  if (MOZ_UNLIKELY(!editingHost)) {
    return Err(NS_ERROR_EDITOR_NO_EDITABLE_RANGE);
  }

  // Remember that we did a selection deletion.  Used by
  // CreateStyleForInsertText()
  TopLevelEditSubActionDataRef().mDidDeleteSelection = true;

  if (MOZ_UNLIKELY(IsEmpty())) {
    return EditActionResult::CanceledResult();
  }

  // First check for table selection mode.  If so, hand off to table editor.
  if (HTMLEditUtils::IsInTableCellSelectionMode(SelectionRef())) {
    nsresult rv = DeleteTableCellContentsWithTransaction();
    if (NS_WARN_IF(Destroyed())) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteTableCellContentsWithTransaction() failed");
      return Err(rv);
    }
    return EditActionResult::HandledResult();
  }

  AutoRangeArray rangesToDelete(SelectionRef());
  rangesToDelete.EnsureOnlyEditableRanges(*editingHost);
  if (MOZ_UNLIKELY(rangesToDelete.Ranges().IsEmpty())) {
    NS_WARNING(
        "There is no range which we can delete entire the ranges or around the "
        "caret");
    return Err(NS_ERROR_EDITOR_NO_EDITABLE_RANGE);
  }
  AutoDeleteRangesHandler deleteHandler;
  Result<EditActionResult, nsresult> result = deleteHandler.Run(
      *this, aDirectionAndAmount, aStripWrappers, rangesToDelete, *editingHost);
  if (MOZ_UNLIKELY(result.isErr()) || result.inspect().Canceled()) {
    NS_WARNING_ASSERTION(result.isOk(),
                         "AutoDeleteRangesHandler::Run() failed");
    return result;
  }

  // XXX At here, selection may have no range because of mutation event
  //     listeners can do anything so that we should just return NS_OK instead
  //     of returning error.
  const auto atNewStartOfSelection =
      GetFirstSelectionStartPoint<EditorDOMPoint>();
  if (NS_WARN_IF(!atNewStartOfSelection.IsSet())) {
    return Err(NS_ERROR_FAILURE);
  }
  if (atNewStartOfSelection.IsInContentNode()) {
    nsresult rv = DeleteMostAncestorMailCiteElementIfEmpty(
        MOZ_KnownLive(*atNewStartOfSelection.ContainerAs<nsIContent>()));
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::DeleteMostAncestorMailCiteElementIfEmpty() failed");
      return Err(rv);
    }
  }
  return EditActionResult::HandledResult();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDelete(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete, const Element& aEditingHost) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());

  mOriginalDirectionAndAmount = aDirectionAndAmount;
  mOriginalStripWrappers = nsIEditor::eNoStrip;

  if (aHTMLEditor.mPaddingBRElementForEmptyEditor) {
    nsresult rv = aRangesToDelete.Collapse(
        EditorRawDOMPoint(aHTMLEditor.mPaddingBRElementForEmptyEditor));
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  SelectionWasCollapsed selectionWasCollapsed = aRangesToDelete.IsCollapsed()
                                                    ? SelectionWasCollapsed::Yes
                                                    : SelectionWasCollapsed::No;
  if (selectionWasCollapsed == SelectionWasCollapsed::Yes) {
    const auto startPoint =
        aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!startPoint.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    RefPtr<Element> editingHost = aHTMLEditor.ComputeEditingHost();
    if (NS_WARN_IF(!editingHost)) {
      return NS_ERROR_FAILURE;
    }
    if (startPoint.IsInContentNode()) {
      AutoEmptyBlockAncestorDeleter deleter;
      if (deleter.ScanEmptyBlockInclusiveAncestor(
              aHTMLEditor, *startPoint.ContainerAs<nsIContent>())) {
        nsresult rv = deleter.ComputeTargetRanges(
            aHTMLEditor, aDirectionAndAmount, *editingHost, aRangesToDelete);
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rv),
            "AutoEmptyBlockAncestorDeleter::ComputeTargetRanges() failed");
        return rv;
      }
    }

    // We shouldn't update caret bidi level right now, but we need to check
    // whether the deletion will be canceled or not.
    AutoCaretBidiLevelManager bidiLevelManager(aHTMLEditor, aDirectionAndAmount,
                                               startPoint);
    if (bidiLevelManager.Failed()) {
      NS_WARNING(
          "EditorBase::AutoCaretBidiLevelManager failed to initialize itself");
      return NS_ERROR_FAILURE;
    }
    if (bidiLevelManager.Canceled()) {
      return NS_SUCCESS_DOM_NO_OPERATION;
    }

    // AutoRangeArray::ExtendAnchorFocusRangeFor() will use `nsFrameSelection`
    // to extend the range for deletion.  But if focus event doesn't receive
    // yet, ancestor isn't set.  So we must set root element of editor to
    // ancestor temporarily.
    AutoSetTemporaryAncestorLimiter autoSetter(
        aHTMLEditor, aHTMLEditor.SelectionRef(), *startPoint.GetContainer(),
        &aRangesToDelete);

    Result<nsIEditor::EDirection, nsresult> extendResult =
        aRangesToDelete.ExtendAnchorFocusRangeFor(aHTMLEditor,
                                                  aDirectionAndAmount);
    if (extendResult.isErr()) {
      NS_WARNING("AutoRangeArray::ExtendAnchorFocusRangeFor() failed");
      return extendResult.unwrapErr();
    }

    // For compatibility with other browsers, we should set target ranges
    // to start from and/or end after an atomic content rather than start
    // from preceding text node end nor end at following text node start.
    Result<bool, nsresult> shrunkenResult =
        aRangesToDelete.ShrinkRangesIfStartFromOrEndAfterAtomicContent(
            aHTMLEditor, aDirectionAndAmount,
            AutoRangeArray::IfSelectingOnlyOneAtomicContent::Collapse,
            editingHost);
    if (shrunkenResult.isErr()) {
      NS_WARNING(
          "AutoRangeArray::ShrinkRangesIfStartFromOrEndAfterAtomicContent() "
          "failed");
      return shrunkenResult.unwrapErr();
    }

    if (!shrunkenResult.inspect() || !aRangesToDelete.IsCollapsed()) {
      aDirectionAndAmount = extendResult.unwrap();
    }

    if (aDirectionAndAmount == nsIEditor::eNone) {
      MOZ_ASSERT(aRangesToDelete.Ranges().Length() == 1);
      if (!CanFallbackToDeleteRangesWithTransaction(aRangesToDelete)) {
        // XXX In this case, do we need to modify the range again?
        return NS_SUCCESS_DOM_NO_OPERATION;
      }
      nsresult rv = FallbackToComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aRangesToDelete);
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "AutoDeleteRangesHandler::"
          "FallbackToComputeRangesToDeleteRangesWithTransaction() failed");
      return rv;
    }

    if (aRangesToDelete.IsCollapsed()) {
      const auto caretPoint =
          aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>();
      if (MOZ_UNLIKELY(NS_WARN_IF(!caretPoint.IsInContentNode()))) {
        return NS_ERROR_FAILURE;
      }
      if (!EditorUtils::IsEditableContent(*caretPoint.ContainerAs<nsIContent>(),
                                          EditorType::HTML)) {
        return NS_SUCCESS_DOM_NO_OPERATION;
      }
      WSRunScanner wsRunScannerAtCaret(editingHost, caretPoint);
      WSScanResult scanFromCaretPointResult =
          aDirectionAndAmount == nsIEditor::eNext
              ? wsRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(
                    caretPoint)
              : wsRunScannerAtCaret.ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                    caretPoint);
      if (scanFromCaretPointResult.Failed()) {
        NS_WARNING(
            "WSRunScanner::Scan(Next|Previous)VisibleNodeOrBlockBoundaryFrom() "
            "failed");
        return NS_ERROR_FAILURE;
      }
      if (!scanFromCaretPointResult.GetContent()) {
        return NS_SUCCESS_DOM_NO_OPERATION;
      }

      if (scanFromCaretPointResult.ReachedBRElement()) {
        if (scanFromCaretPointResult.BRElementPtr() ==
            wsRunScannerAtCaret.GetEditingHost()) {
          return NS_OK;
        }
        if (!EditorUtils::IsEditableContent(
                *scanFromCaretPointResult.BRElementPtr(), EditorType::HTML)) {
          return NS_SUCCESS_DOM_NO_OPERATION;
        }
        if (HTMLEditUtils::IsInvisibleBRElement(
                *scanFromCaretPointResult.BRElementPtr())) {
          EditorDOMPoint newCaretPosition =
              aDirectionAndAmount == nsIEditor::eNext
                  ? EditorDOMPoint::After(
                        *scanFromCaretPointResult.BRElementPtr())
                  : EditorDOMPoint(scanFromCaretPointResult.BRElementPtr());
          if (NS_WARN_IF(!newCaretPosition.IsSet())) {
            return NS_ERROR_FAILURE;
          }
          AutoHideSelectionChanges blockSelectionListeners(
              aHTMLEditor.SelectionRef());
          nsresult rv = aHTMLEditor.CollapseSelectionTo(newCaretPosition);
          if (MOZ_UNLIKELY(NS_FAILED(rv))) {
            NS_WARNING("EditorBase::CollapseSelectionTo() failed");
            return NS_ERROR_FAILURE;
          }
          if (NS_WARN_IF(!aHTMLEditor.SelectionRef().RangeCount())) {
            return NS_ERROR_UNEXPECTED;
          }
          aRangesToDelete.Initialize(aHTMLEditor.SelectionRef());
          AutoDeleteRangesHandler anotherHandler(this);
          rv = anotherHandler.ComputeRangesToDelete(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete, aEditingHost);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "Recursive AutoDeleteRangesHandler::ComputeRangesToDelete() "
              "failed");

          rv = aHTMLEditor.CollapseSelectionTo(caretPoint);
          if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
            NS_WARNING(
                "EditorBase::CollapseSelectionTo() caused destroying the "
                "editor");
            return NS_ERROR_EDITOR_DESTROYED;
          }
          NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                               "EditorBase::CollapseSelectionTo() failed to "
                               "restore original selection, but ignored");

          MOZ_ASSERT(aRangesToDelete.Ranges().Length() == 1);
          // If the range is collapsed, there is no content which should
          // be removed together.  In this case, only the invisible `<br>`
          // element should be selected.
          if (aRangesToDelete.IsCollapsed()) {
            nsresult rv = aRangesToDelete.SelectNode(
                *scanFromCaretPointResult.BRElementPtr());
            NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                                 "AutoRangeArray::SelectNode() failed");
            return rv;
          }

          // Otherwise, extend the range to contain the invisible `<br>`
          // element.
          if (EditorRawDOMPoint(scanFromCaretPointResult.BRElementPtr())
                  .IsBefore(
                      aRangesToDelete
                          .GetFirstRangeStartPoint<EditorRawDOMPoint>())) {
            nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
                EditorRawDOMPoint(scanFromCaretPointResult.BRElementPtr())
                    .ToRawRangeBoundary(),
                aRangesToDelete.FirstRangeRef()->EndRef());
            NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                                 "nsRange::SetStartAndEnd() failed");
            return rv;
          }
          if (aRangesToDelete.GetFirstRangeEndPoint<EditorRawDOMPoint>()
                  .IsBefore(EditorRawDOMPoint::After(
                      *scanFromCaretPointResult.BRElementPtr()))) {
            nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
                aRangesToDelete.FirstRangeRef()->StartRef(),
                EditorRawDOMPoint::After(
                    *scanFromCaretPointResult.BRElementPtr())
                    .ToRawRangeBoundary());
            NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                                 "nsRange::SetStartAndEnd() failed");
            return rv;
          }
          NS_WARNING("Was the invisible `<br>` element selected?");
          return NS_OK;
        }
      }

      nsresult rv = ComputeRangesToDeleteAroundCollapsedRanges(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete,
          wsRunScannerAtCaret, scanFromCaretPointResult, aEditingHost);
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "AutoDeleteRangesHandler::ComputeRangesToDeleteAroundCollapsedRanges("
          ") failed");
      return rv;
    }
  }

  nsresult rv = ComputeRangesToDeleteNonCollapsedRanges(
      aHTMLEditor, aDirectionAndAmount, aRangesToDelete, selectionWasCollapsed,
      aEditingHost);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "ComputeRangesToDeleteNonCollapsedRanges() failed");
  return rv;
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::Run(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
    const Element& aEditingHost) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aStripWrappers == nsIEditor::eStrip ||
             aStripWrappers == nsIEditor::eNoStrip);
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());

  mOriginalDirectionAndAmount = aDirectionAndAmount;
  mOriginalStripWrappers = aStripWrappers;

  if (MOZ_UNLIKELY(aHTMLEditor.IsEmpty())) {
    return EditActionResult::CanceledResult();
  }

  // selectionWasCollapsed is used later to determine whether we should join
  // blocks in HandleDeleteNonCollapsedRanges(). We don't really care about
  // collapsed because it will be modified by
  // AutoRangeArray::ExtendAnchorFocusRangeFor() later.
  // AutoBlockElementsJoiner::AutoInclusiveAncestorBlockElementsJoiner should
  // happen if the original selection is collapsed and the cursor is at the end
  // of a block element, in which case
  // AutoRangeArray::ExtendAnchorFocusRangeFor() would always make the selection
  // not collapsed.
  SelectionWasCollapsed selectionWasCollapsed = aRangesToDelete.IsCollapsed()
                                                    ? SelectionWasCollapsed::Yes
                                                    : SelectionWasCollapsed::No;

  if (selectionWasCollapsed == SelectionWasCollapsed::Yes) {
    const auto startPoint =
        aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!startPoint.IsSet())) {
      return Err(NS_ERROR_FAILURE);
    }

    // If we are inside an empty block, delete it.
    if (startPoint.IsInContentNode()) {
#ifdef DEBUG
      nsMutationGuard debugMutation;
#endif  // #ifdef DEBUG
      AutoEmptyBlockAncestorDeleter deleter;
      if (deleter.ScanEmptyBlockInclusiveAncestor(
              aHTMLEditor, *startPoint.ContainerAs<nsIContent>())) {
        Result<EditActionResult, nsresult> result =
            deleter.Run(aHTMLEditor, aDirectionAndAmount);
        if (MOZ_UNLIKELY(result.isErr()) || result.inspect().Handled()) {
          NS_WARNING_ASSERTION(result.isOk(),
                               "AutoEmptyBlockAncestorDeleter::Run() failed");
          return result;
        }
      }
      MOZ_ASSERT(!debugMutation.Mutated(0),
                 "AutoEmptyBlockAncestorDeleter shouldn't modify the DOM tree "
                 "if it returns not handled nor error");
    }

    // Test for distance between caret and text that will be deleted.
    // Note that this call modifies `nsFrameSelection` without modifying
    // `Selection`.  However, it does not have problem for now because
    // it'll be referred by `AutoRangeArray::ExtendAnchorFocusRangeFor()`
    // before modifying `Selection`.
    // XXX This looks odd.  `ExtendAnchorFocusRangeFor()` will extend
    //     anchor-focus range, but here refers the first range.
    AutoCaretBidiLevelManager bidiLevelManager(aHTMLEditor, aDirectionAndAmount,
                                               startPoint);
    if (MOZ_UNLIKELY(bidiLevelManager.Failed())) {
      NS_WARNING(
          "EditorBase::AutoCaretBidiLevelManager failed to initialize itself");
      return Err(NS_ERROR_FAILURE);
    }
    bidiLevelManager.MaybeUpdateCaretBidiLevel(aHTMLEditor);
    if (bidiLevelManager.Canceled()) {
      return EditActionResult::CanceledResult();
    }

    // AutoRangeArray::ExtendAnchorFocusRangeFor() will use `nsFrameSelection`
    // to extend the range for deletion.  But if focus event doesn't receive
    // yet, ancestor isn't set.  So we must set root element of editor to
    // ancestor temporarily.
    AutoSetTemporaryAncestorLimiter autoSetter(
        aHTMLEditor, aHTMLEditor.SelectionRef(), *startPoint.GetContainer(),
        &aRangesToDelete);

    // Calling `ExtendAnchorFocusRangeFor()` and
    // `ShrinkRangesIfStartFromOrEndAfterAtomicContent()` may move caret to
    // the container of deleting atomic content.  However, it may be different
    // from the original caret's container.  The original caret container may
    // be important to put caret after deletion so that let's cache the
    // original position.
    Maybe<EditorDOMPoint> caretPoint;
    if (aRangesToDelete.IsCollapsed() && !aRangesToDelete.Ranges().IsEmpty()) {
      caretPoint =
          Some(aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>());
      if (NS_WARN_IF(!caretPoint.ref().IsInContentNode())) {
        return Err(NS_ERROR_FAILURE);
      }
    }

    Result<nsIEditor::EDirection, nsresult> extendResult =
        aRangesToDelete.ExtendAnchorFocusRangeFor(aHTMLEditor,
                                                  aDirectionAndAmount);
    if (MOZ_UNLIKELY(extendResult.isErr())) {
      NS_WARNING("AutoRangeArray::ExtendAnchorFocusRangeFor() failed");
      return extendResult.propagateErr();
    }
    if (caretPoint.isSome() &&
        MOZ_UNLIKELY(!caretPoint.ref().IsSetAndValid())) {
      NS_WARNING("The caret position became invalid");
      return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
    }

    // If there is only one range and it selects an atomic content, we should
    // delete it with collapsed range path for making consistent behavior
    // between both cases, the content is selected case and caret is at it or
    // after it case.
    Result<bool, nsresult> shrunkenResult =
        aRangesToDelete.ShrinkRangesIfStartFromOrEndAfterAtomicContent(
            aHTMLEditor, aDirectionAndAmount,
            AutoRangeArray::IfSelectingOnlyOneAtomicContent::Collapse,
            &aEditingHost);
    if (MOZ_UNLIKELY(shrunkenResult.isErr())) {
      NS_WARNING(
          "AutoRangeArray::ShrinkRangesIfStartFromOrEndAfterAtomicContent() "
          "failed");
      return shrunkenResult.propagateErr();
    }

    if (!shrunkenResult.inspect() || !aRangesToDelete.IsCollapsed()) {
      aDirectionAndAmount = extendResult.unwrap();
    }

    if (aDirectionAndAmount == nsIEditor::eNone) {
      MOZ_ASSERT(aRangesToDelete.Ranges().Length() == 1);
      if (!CanFallbackToDeleteRangesWithTransaction(aRangesToDelete)) {
        return EditActionResult::IgnoredResult();
      }
      Result<EditActionResult, nsresult> result =
          FallbackToDeleteRangesWithTransaction(aHTMLEditor, aRangesToDelete);
      NS_WARNING_ASSERTION(result.isOk(),
                           "AutoDeleteRangesHandler::"
                           "FallbackToDeleteRangesWithTransaction() failed");
      return result;
    }

    if (aRangesToDelete.IsCollapsed()) {
      // Use the original caret position for handling the deletion around
      // collapsed range because the container may be different from the
      // new collapsed position's container.
      if (!EditorUtils::IsEditableContent(
              *caretPoint.ref().ContainerAs<nsIContent>(), EditorType::HTML)) {
        return EditActionResult::CanceledResult();
      }
      WSRunScanner wsRunScannerAtCaret(&aEditingHost, caretPoint.ref());
      WSScanResult scanFromCaretPointResult =
          aDirectionAndAmount == nsIEditor::eNext
              ? wsRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(
                    caretPoint.ref())
              : wsRunScannerAtCaret.ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                    caretPoint.ref());
      if (MOZ_UNLIKELY(scanFromCaretPointResult.Failed())) {
        NS_WARNING(
            "WSRunScanner::Scan(Next|Previous)VisibleNodeOrBlockBoundaryFrom() "
            "failed");
        return Err(NS_ERROR_FAILURE);
      }
      if (!scanFromCaretPointResult.GetContent()) {
        return EditActionResult::CanceledResult();
      }
      // Short circuit for invisible breaks.  delete them and recurse.
      if (scanFromCaretPointResult.ReachedBRElement()) {
        if (scanFromCaretPointResult.BRElementPtr() == &aEditingHost) {
          return EditActionResult::HandledResult();
        }
        if (!EditorUtils::IsEditableContent(
                *scanFromCaretPointResult.BRElementPtr(), EditorType::HTML)) {
          return EditActionResult::CanceledResult();
        }
        if (HTMLEditUtils::IsInvisibleBRElement(
                *scanFromCaretPointResult.BRElementPtr())) {
          // TODO: We should extend the range to delete again before/after
          //       the caret point and use `HandleDeleteNonCollapsedRanges()`
          //       instead after we would create delete range computation
          //       method at switching to the new white-space normalizer.
          nsresult rv = WhiteSpaceVisibilityKeeper::
              DeleteContentNodeAndJoinTextNodesAroundIt(
                  aHTMLEditor,
                  MOZ_KnownLive(*scanFromCaretPointResult.BRElementPtr()),
                  caretPoint.ref());
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "WhiteSpaceVisibilityKeeper::"
                "DeleteContentNodeAndJoinTextNodesAroundIt() failed");
            return Err(rv);
          }
          if (MOZ_UNLIKELY(aHTMLEditor.SelectionRef().RangeCount() != 1u)) {
            NS_WARNING(
                "Selection was unexpected after removing an invisible `<br>` "
                "element");
            return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
          }
          AutoRangeArray rangesToDelete(aHTMLEditor.SelectionRef());
          caretPoint =
              Some(aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>());
          if (MOZ_UNLIKELY(!caretPoint.ref().IsSet())) {
            NS_WARNING(
                "New selection after deleting invisible `<br>` element was "
                "invalid");
            return Err(NS_ERROR_FAILURE);
          }
          if (aHTMLEditor.MayHaveMutationEventListeners(
                  NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED |
                  NS_EVENT_BITS_MUTATION_NODEREMOVED |
                  NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT)) {
            // Let's check whether there is new invisible `<br>` element
            // for avoiding infinite recursive calls.
            WSRunScanner wsRunScannerAtCaret(&aEditingHost, caretPoint.ref());
            WSScanResult scanFromCaretPointResult =
                aDirectionAndAmount == nsIEditor::eNext
                    ? wsRunScannerAtCaret
                          .ScanNextVisibleNodeOrBlockBoundaryFrom(
                              caretPoint.ref())
                    : wsRunScannerAtCaret
                          .ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                              caretPoint.ref());
            if (MOZ_UNLIKELY(scanFromCaretPointResult.Failed())) {
              NS_WARNING(
                  "WSRunScanner::Scan(Next|Previous)"
                  "VisibleNodeOrBlockBoundaryFrom() failed");
              return Err(NS_ERROR_FAILURE);
            }
            if (MOZ_UNLIKELY(
                    scanFromCaretPointResult.ReachedInvisibleBRElement())) {
              return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
            }
          }
          AutoDeleteRangesHandler anotherHandler(this);
          Result<EditActionResult, nsresult> result =
              anotherHandler.Run(aHTMLEditor, aDirectionAndAmount,
                                 aStripWrappers, rangesToDelete, aEditingHost);
          NS_WARNING_ASSERTION(
              result.isOk(), "Recursive AutoDeleteRangesHandler::Run() failed");
          return result;
        }
      }

      Result<EditActionResult, nsresult> result =
          HandleDeleteAroundCollapsedRanges(
              aHTMLEditor, aDirectionAndAmount, aStripWrappers, aRangesToDelete,
              wsRunScannerAtCaret, scanFromCaretPointResult, aEditingHost);
      NS_WARNING_ASSERTION(result.isOk(),
                           "AutoDeleteRangesHandler::"
                           "HandleDeleteAroundCollapsedRanges() failed");
      return result;
    }
  }

  Result<EditActionResult, nsresult> result = HandleDeleteNonCollapsedRanges(
      aHTMLEditor, aDirectionAndAmount, aStripWrappers, aRangesToDelete,
      selectionWasCollapsed, aEditingHost);
  NS_WARNING_ASSERTION(
      result.isOk(),
      "AutoDeleteRangesHandler::HandleDeleteNonCollapsedRanges() failed");
  return result;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteAroundCollapsedRanges(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete, const WSRunScanner& aWSRunScannerAtCaret,
    const WSScanResult& aScanFromCaretPointResult,
    const Element& aEditingHost) const {
  if (aScanFromCaretPointResult.InCollapsibleWhiteSpaces() ||
      aScanFromCaretPointResult.InNonCollapsibleCharacters() ||
      aScanFromCaretPointResult.ReachedPreformattedLineBreak()) {
    nsresult rv = aRangesToDelete.Collapse(
        aScanFromCaretPointResult.Point<EditorRawDOMPoint>());
    if (MOZ_UNLIKELY(NS_FAILED(rv))) {
      NS_WARNING("AutoRangeArray::Collapse() failed");
      return NS_ERROR_FAILURE;
    }
    rv = ComputeRangesToDeleteTextAroundCollapsedRanges(
        aWSRunScannerAtCaret.GetEditingHost(), aDirectionAndAmount,
        aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::"
        "ComputeRangesToDeleteTextAroundCollapsedRanges() failed");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedSpecialContent() ||
      aScanFromCaretPointResult.ReachedBRElement() ||
      aScanFromCaretPointResult.ReachedNonEditableOtherBlockElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return NS_OK;
    }
    nsIContent* atomicContent = GetAtomicContentToDelete(
        aDirectionAndAmount, aWSRunScannerAtCaret, aScanFromCaretPointResult);
    if (!HTMLEditUtils::IsRemovableNode(*atomicContent)) {
      NS_WARNING(
          "AutoDeleteRangesHandler::GetAtomicContentToDelete() cannot find "
          "removable atomic content");
      return NS_ERROR_FAILURE;
    }
    nsresult rv = ComputeRangesToDeleteAtomicContent(
        aWSRunScannerAtCaret.GetEditingHost(), *atomicContent, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent() failed");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedHRElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return NS_OK;
    }
    nsresult rv =
        ComputeRangesToDeleteHRElement(aHTMLEditor, aDirectionAndAmount,
                                       *aScanFromCaretPointResult.ElementPtr(),
                                       aWSRunScannerAtCaret.ScanStartRef(),
                                       aWSRunScannerAtCaret, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::ComputeRangesToDeleteHRElement() failed");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedOtherBlockElement()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return NS_ERROR_FAILURE;
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtOtherBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef(), aWSRunScannerAtCaret)) {
      return NS_SUCCESS_DOM_NO_OPERATION;
    }
    nsresult rv = joiner.ComputeRangesToDelete(
        aHTMLEditor, aDirectionAndAmount, aWSRunScannerAtCaret.ScanStartRef(),
        aRangesToDelete, aEditingHost);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoBlockElementsJoiner::ComputeRangesToDelete() "
                         "failed (other block boundary)");
    return rv;
  }

  if (aScanFromCaretPointResult.ReachedCurrentBlockBoundary()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return NS_ERROR_FAILURE;
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtCurrentBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef())) {
      return NS_SUCCESS_DOM_NO_OPERATION;
    }
    nsresult rv = joiner.ComputeRangesToDelete(
        aHTMLEditor, aDirectionAndAmount, aWSRunScannerAtCaret.ScanStartRef(),
        aRangesToDelete, aEditingHost);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoBlockElementsJoiner::ComputeRangesToDelete() "
                         "failed (current block boundary)");
    return rv;
  }

  return NS_OK;
}

Result<EditActionResult, nsresult>
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteAroundCollapsedRanges(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
    const WSRunScanner& aWSRunScannerAtCaret,
    const WSScanResult& aScanFromCaretPointResult,
    const Element& aEditingHost) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(aDirectionAndAmount != nsIEditor::eNone);
  MOZ_ASSERT(aWSRunScannerAtCaret.ScanStartRef().IsInContentNode());
  MOZ_ASSERT(EditorUtils::IsEditableContent(
      *aWSRunScannerAtCaret.ScanStartRef().ContainerAs<nsIContent>(),
      EditorType::HTML));

  if (StaticPrefs::editor_white_space_normalization_blink_compatible()) {
    if (aScanFromCaretPointResult.InCollapsibleWhiteSpaces() ||
        aScanFromCaretPointResult.InNonCollapsibleCharacters() ||
        aScanFromCaretPointResult.ReachedPreformattedLineBreak()) {
      nsresult rv = aRangesToDelete.Collapse(
          aScanFromCaretPointResult.Point<EditorRawDOMPoint>());
      if (NS_FAILED(rv)) {
        NS_WARNING("AutoRangeArray::Collapse() failed");
        return Err(NS_ERROR_FAILURE);
      }
      Result<EditActionResult, nsresult> result =
          HandleDeleteTextAroundCollapsedRanges(
              aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
      NS_WARNING_ASSERTION(result.isOk(),
                           "AutoDeleteRangesHandler::"
                           "HandleDeleteTextAroundCollapsedRanges() failed");
      return result;
    }
  }

  if (aScanFromCaretPointResult.InCollapsibleWhiteSpaces() ||
      aScanFromCaretPointResult.ReachedPreformattedLineBreak()) {
    Result<EditActionResult, nsresult> result =
        HandleDeleteCollapsedSelectionAtWhiteSpaces(
            aHTMLEditor, aDirectionAndAmount,
            aWSRunScannerAtCaret.ScanStartRef());
    NS_WARNING_ASSERTION(
        result.isOk(),
        "AutoDeleteRangesHandler::HandleDeleteCollapsedSelectionAtWhiteSpaces()"
        " failed");
    return result;
  }

  if (aScanFromCaretPointResult.InNonCollapsibleCharacters()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsText())) {
      return Err(NS_ERROR_FAILURE);
    }
    Result<EditActionResult, nsresult> result =
        HandleDeleteCollapsedSelectionAtVisibleChar(
            aHTMLEditor, aDirectionAndAmount,
            aScanFromCaretPointResult.Point<EditorDOMPoint>());
    NS_WARNING_ASSERTION(result.isOk(),
                         "AutoDeleteRangesHandler::"
                         "HandleDeleteCollapsedSelectionAtVisibleChar() "
                         "failed");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedSpecialContent() ||
      aScanFromCaretPointResult.ReachedBRElement() ||
      aScanFromCaretPointResult.ReachedNonEditableOtherBlockElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return EditActionResult::HandledResult();
    }
    nsCOMPtr<nsIContent> atomicContent = GetAtomicContentToDelete(
        aDirectionAndAmount, aWSRunScannerAtCaret, aScanFromCaretPointResult);
    if (MOZ_UNLIKELY(!HTMLEditUtils::IsRemovableNode(*atomicContent))) {
      NS_WARNING(
          "AutoDeleteRangesHandler::GetAtomicContentToDelete() cannot find "
          "removable atomic content");
      return Err(NS_ERROR_FAILURE);
    }
    Result<EditActionResult, nsresult> result = HandleDeleteAtomicContent(
        aHTMLEditor, *atomicContent, aWSRunScannerAtCaret.ScanStartRef(),
        aWSRunScannerAtCaret);
    NS_WARNING_ASSERTION(
        result.isOk(),
        "AutoDeleteRangesHandler::HandleDeleteAtomicContent() failed");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedHRElement()) {
    if (aScanFromCaretPointResult.GetContent() ==
        aWSRunScannerAtCaret.GetEditingHost()) {
      return EditActionResult::HandledResult();
    }
    Result<EditActionResult, nsresult> result = HandleDeleteHRElement(
        aHTMLEditor, aDirectionAndAmount,
        MOZ_KnownLive(*aScanFromCaretPointResult.ElementPtr()),
        aWSRunScannerAtCaret.ScanStartRef(), aWSRunScannerAtCaret);
    NS_WARNING_ASSERTION(
        result.isOk(),
        "AutoDeleteRangesHandler::HandleDeleteHRElement() failed");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedOtherBlockElement()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return Err(NS_ERROR_FAILURE);
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtOtherBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef(), aWSRunScannerAtCaret)) {
      return EditActionResult::CanceledResult();
    }
    Result<EditActionResult, nsresult> result = joiner.Run(
        aHTMLEditor, aDirectionAndAmount, aStripWrappers,
        aWSRunScannerAtCaret.ScanStartRef(), aRangesToDelete, aEditingHost);
    NS_WARNING_ASSERTION(
        result.isOk(),
        "AutoBlockElementsJoiner::Run() failed (other block boundary)");
    return result;
  }

  if (aScanFromCaretPointResult.ReachedCurrentBlockBoundary()) {
    if (NS_WARN_IF(!aScanFromCaretPointResult.GetContent()->IsElement())) {
      return Err(NS_ERROR_FAILURE);
    }
    AutoBlockElementsJoiner joiner(*this);
    if (!joiner.PrepareToDeleteAtCurrentBlockBoundary(
            aHTMLEditor, aDirectionAndAmount,
            *aScanFromCaretPointResult.ElementPtr(),
            aWSRunScannerAtCaret.ScanStartRef())) {
      return EditActionResult::CanceledResult();
    }
    Result<EditActionResult, nsresult> result = joiner.Run(
        aHTMLEditor, aDirectionAndAmount, aStripWrappers,
        aWSRunScannerAtCaret.ScanStartRef(), aRangesToDelete, aEditingHost);
    NS_WARNING_ASSERTION(
        result.isOk(),
        "AutoBlockElementsJoiner::Run() failed (current block boundary)");
    return result;
  }

  MOZ_ASSERT_UNREACHABLE("New type of reached content hasn't been handled yet");
  return EditActionResult::IgnoredResult();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::
    ComputeRangesToDeleteTextAroundCollapsedRanges(
        Element* aEditingHost, nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aDirectionAndAmount == nsIEditor::eNext ||
             aDirectionAndAmount == nsIEditor::ePrevious);

  const auto caretPosition =
      aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>();
  MOZ_ASSERT(caretPosition.IsSetAndValid());
  if (MOZ_UNLIKELY(NS_WARN_IF(!caretPosition.IsInContentNode()))) {
    return NS_ERROR_FAILURE;
  }

  EditorDOMRangeInTexts rangeToDelete;
  if (aDirectionAndAmount == nsIEditor::eNext) {
    Result<EditorDOMRangeInTexts, nsresult> result =
        WSRunScanner::GetRangeInTextNodesToForwardDeleteFrom(aEditingHost,
                                                             caretPosition);
    if (result.isErr()) {
      NS_WARNING(
          "WSRunScanner::GetRangeInTextNodesToForwardDeleteFrom() failed");
      return result.unwrapErr();
    }
    rangeToDelete = result.unwrap();
    if (!rangeToDelete.IsPositioned()) {
      return NS_OK;  // no range to delete, but consume it.
    }
  } else {
    Result<EditorDOMRangeInTexts, nsresult> result =
        WSRunScanner::GetRangeInTextNodesToBackspaceFrom(aEditingHost,
                                                         caretPosition);
    if (result.isErr()) {
      NS_WARNING("WSRunScanner::GetRangeInTextNodesToBackspaceFrom() failed");
      return result.unwrapErr();
    }
    rangeToDelete = result.unwrap();
    if (!rangeToDelete.IsPositioned()) {
      return NS_OK;  // no range to delete, but consume it.
    }
  }

  nsresult rv = aRangesToDelete.SetStartAndEnd(rangeToDelete.StartRef(),
                                               rangeToDelete.EndRef());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoArrayRanges::SetStartAndEnd() failed");
  return rv;
}

Result<EditActionResult, nsresult>
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteTextAroundCollapsedRanges(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aDirectionAndAmount == nsIEditor::eNext ||
             aDirectionAndAmount == nsIEditor::ePrevious);

  RefPtr<Element> editingHost = aHTMLEditor.ComputeEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return Err(NS_ERROR_FAILURE);
  }

  nsresult rv = ComputeRangesToDeleteTextAroundCollapsedRanges(
      editingHost, aDirectionAndAmount, aRangesToDelete);
  if (NS_FAILED(rv)) {
    return Err(NS_ERROR_FAILURE);
  }
  if (MOZ_UNLIKELY(aRangesToDelete.IsCollapsed())) {
    return EditActionResult::HandledResult();  // no range to delete
  }

  // FYI: rangeToDelete does not contain newly empty inline ancestors which
  //      are removed by DeleteTextAndNormalizeSurroundingWhiteSpaces().
  //      So, if `getTargetRanges()` needs to include parent empty elements,
  //      we need to extend the range with
  //      HTMLEditUtils::GetMostDistantAncestorEditableEmptyInlineElement().
  EditorRawDOMRange rangeToDelete(aRangesToDelete.FirstRangeRef());
  if (MOZ_UNLIKELY(!rangeToDelete.IsInTextNodes())) {
    NS_WARNING("The extended range to delete character was not in text nodes");
    return Err(NS_ERROR_FAILURE);
  }

  AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
  Result<EditorDOMPoint, nsresult> result =
      aHTMLEditor.DeleteTextAndNormalizeSurroundingWhiteSpaces(
          rangeToDelete.StartRef().AsInText(),
          rangeToDelete.EndRef().AsInText(),
          TreatEmptyTextNodes::RemoveAllEmptyInlineAncestors,
          aDirectionAndAmount == nsIEditor::eNext ? DeleteDirection::Forward
                                                  : DeleteDirection::Backward);
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidNormalizeWhitespaces = true;
  if (MOZ_UNLIKELY(result.isErr())) {
    NS_WARNING(
        "HTMLEditor::DeleteTextAndNormalizeSurroundingWhiteSpaces() failed");
    return result.propagateErr();
  }
  const EditorDOMPoint& newCaretPosition = result.inspect();
  MOZ_ASSERT(newCaretPosition.IsSetAndValid());

  rv = aHTMLEditor.CollapseSelectionTo(newCaretPosition);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed, but ignored");
  return EditActionResult::HandledResult();
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    HandleDeleteCollapsedSelectionAtWhiteSpaces(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aPointToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!StaticPrefs::editor_white_space_normalization_blink_compatible());

  if (aDirectionAndAmount == nsIEditor::eNext) {
    nsresult rv = WhiteSpaceVisibilityKeeper::DeleteInclusiveNextWhiteSpace(
        aHTMLEditor, aPointToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::DeleteInclusiveNextWhiteSpace() failed");
      return Err(rv);
    }
  } else {
    nsresult rv = WhiteSpaceVisibilityKeeper::DeletePreviousWhiteSpace(
        aHTMLEditor, aPointToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::DeletePreviousWhiteSpace() failed");
      return Err(rv);
    }
  }
  const auto newCaretPosition =
      aHTMLEditor.GetFirstSelectionStartPoint<EditorDOMPoint>();
  if (MOZ_UNLIKELY(!newCaretPosition.IsSet())) {
    NS_WARNING("There was no selection range");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }
  nsresult rv =
      aHTMLEditor.InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
          newCaretPosition);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary()"
        " failed");
    return Err(rv);
  }
  return EditActionResult::HandledResult();
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    HandleDeleteCollapsedSelectionAtVisibleChar(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aPointToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!StaticPrefs::editor_white_space_normalization_blink_compatible());
  MOZ_ASSERT(aPointToDelete.IsSet());
  MOZ_ASSERT(aPointToDelete.IsInTextNode());

  OwningNonNull<Text> visibleTextNode = *aPointToDelete.ContainerAs<Text>();
  EditorDOMPoint startToDelete, endToDelete;
  if (aDirectionAndAmount == nsIEditor::ePrevious) {
    if (MOZ_UNLIKELY(aPointToDelete.IsStartOfContainer())) {
      return Err(NS_ERROR_UNEXPECTED);
    }
    startToDelete = aPointToDelete.PreviousPoint();
    endToDelete = aPointToDelete;
    // Bug 1068979: delete both codepoints if surrogate pair
    if (!startToDelete.IsStartOfContainer()) {
      const nsTextFragment* text = &visibleTextNode->TextFragment();
      if (text->IsLowSurrogateFollowingHighSurrogateAt(
              startToDelete.Offset())) {
        startToDelete.RewindOffset();
      }
    }
  } else {
    RefPtr<const nsRange> range = aHTMLEditor.SelectionRef().GetRangeAt(0);
    if (NS_WARN_IF(!range) ||
        NS_WARN_IF(range->GetStartContainer() !=
                   aPointToDelete.GetContainer()) ||
        NS_WARN_IF(range->GetEndContainer() != aPointToDelete.GetContainer())) {
      return Err(NS_ERROR_FAILURE);
    }
    startToDelete = range->StartRef();
    endToDelete = range->EndRef();
  }
  nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRangeAndTrackPoints(
      aHTMLEditor, &startToDelete, &endToDelete);
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToDeleteRangeAndTrackPoints() "
        "failed");
    return Err(rv);
  }
  if (aHTMLEditor.MayHaveMutationEventListeners(
          NS_EVENT_BITS_MUTATION_NODEREMOVED |
          NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
          NS_EVENT_BITS_MUTATION_ATTRMODIFIED |
          NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED) &&
      (NS_WARN_IF(!startToDelete.IsSetAndValid()) ||
       NS_WARN_IF(!startToDelete.IsInTextNode()) ||
       NS_WARN_IF(!endToDelete.IsSetAndValid()) ||
       NS_WARN_IF(!endToDelete.IsInTextNode()) ||
       NS_WARN_IF(startToDelete.ContainerAs<Text>() != visibleTextNode) ||
       NS_WARN_IF(endToDelete.ContainerAs<Text>() != visibleTextNode) ||
       NS_WARN_IF(startToDelete.Offset() >= endToDelete.Offset()))) {
    NS_WARNING("Mutation event listener changed the DOM tree");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }
  rv = aHTMLEditor.DeleteTextWithTransaction(
      visibleTextNode, startToDelete.Offset(),
      endToDelete.Offset() - startToDelete.Offset());
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
    return Err(rv);
  }

  // XXX When Backspace key is pressed, Chromium removes following empty
  //     text nodes when removing the last character of the non-empty text
  //     node.  However, Edge never removes empty text nodes even if
  //     selection is in the following empty text node(s).  For now, we
  //     should keep our traditional behavior same as Edge for backward
  //     compatibility.
  // XXX When Delete key is pressed, Edge removes all preceding empty
  //     text nodes when removing the first character of the non-empty
  //     text node.  Chromium removes only selected empty text node and
  //     following empty text nodes and the first character of the
  //     non-empty text node.  For now, we should keep our traditional
  //     behavior same as Chromium for backward compatibility.

  rv = DeleteNodeIfInvisibleAndEditableTextNode(aHTMLEditor, visibleTextNode);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode() "
      "failed, but ignored");

  const auto newCaretPosition =
      aHTMLEditor.GetFirstSelectionStartPoint<EditorDOMPoint>();
  if (MOZ_UNLIKELY(!newCaretPosition.IsSet())) {
    NS_WARNING("There was no selection range");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  // XXX `Selection` may be modified by mutation event listeners so
  //     that we should use EditorDOMPoint::AtEndOf(visibleTextNode)
  //     instead.  (Perhaps, we don't and/or shouldn't need to do this
  //     if the text node is preformatted.)
  rv = aHTMLEditor.InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
      newCaretPosition);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary()"
        " failed");
    return Err(rv);
  }

  // Remember that we did a ranged delete for the benefit of
  // AfterEditInner().
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidDeleteNonCollapsedRange = true;

  return EditActionResult::HandledResult();
}

Result<bool, nsresult>
HTMLEditor::AutoDeleteRangesHandler::ShouldDeleteHRElement(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    Element& aHRElement, const EditorDOMPoint& aCaretPoint) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  if (StaticPrefs::editor_hr_element_allow_to_delete_from_following_line()) {
    return true;
  }

  if (aDirectionAndAmount != nsIEditor::ePrevious) {
    return true;
  }

  // Only if the caret is positioned at the end-of-hr-line position, we
  // want to delete the <hr>.
  //
  // In other words, we only want to delete, if our selection position
  // (indicated by aCaretPoint) is the position directly
  // after the <hr>, on the same line as the <hr>.
  //
  // To detect this case we check:
  // aCaretPoint's container == parent of `<hr>` element
  // and
  // aCaretPoint's offset -1 == `<hr>` element offset
  // and
  // interline position is false (left)
  //
  // In any other case we set the position to aCaretPoint's container -1
  // and interlineposition to false, only moving the caret to the
  // end-of-hr-line position.
  EditorRawDOMPoint atHRElement(&aHRElement);

  const InterlinePosition interlinePosition =
      aHTMLEditor.SelectionRef().GetInterlinePosition();
  if (MOZ_UNLIKELY(interlinePosition == InterlinePosition::Undefined)) {
    NS_WARNING("Selection::GetInterlinePosition() failed");
    return Err(NS_ERROR_FAILURE);
  }

  return interlinePosition == InterlinePosition::EndOfLine &&
         aCaretPoint.GetContainer() == atHRElement.GetContainer() &&
         aCaretPoint.Offset() - 1 == atHRElement.Offset();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteHRElement(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    Element& aHRElement, const EditorDOMPoint& aCaretPoint,
    const WSRunScanner& aWSRunScannerAtCaret,
    AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aHRElement.IsHTMLElement(nsGkAtoms::hr));
  MOZ_ASSERT(&aHRElement != aWSRunScannerAtCaret.GetEditingHost());

  Result<bool, nsresult> canDeleteHRElement = ShouldDeleteHRElement(
      aHTMLEditor, aDirectionAndAmount, aHRElement, aCaretPoint);
  if (canDeleteHRElement.isErr()) {
    NS_WARNING("AutoDeleteRangesHandler::ShouldDeleteHRElement() failed");
    return canDeleteHRElement.unwrapErr();
  }
  if (canDeleteHRElement.inspect()) {
    nsresult rv = ComputeRangesToDeleteAtomicContent(
        aWSRunScannerAtCaret.GetEditingHost(), aHRElement, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent() failed");
    return rv;
  }

  WSScanResult forwardScanFromCaretResult =
      aWSRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(aCaretPoint);
  if (forwardScanFromCaretResult.Failed()) {
    NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom() failed");
    return NS_ERROR_FAILURE;
  }
  if (!forwardScanFromCaretResult.ReachedBRElement()) {
    // Restore original caret position if we won't delete anyting.
    nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  // If we'll just move caret position, but if it's followed by a `<br>`
  // element, we'll delete it.
  nsresult rv = ComputeRangesToDeleteAtomicContent(
      aWSRunScannerAtCaret.GetEditingHost(),
      *forwardScanFromCaretResult.ElementPtr(), aRangesToDelete);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent() failed");
  return rv;
}

Result<EditActionResult, nsresult>
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteHRElement(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    Element& aHRElement, const EditorDOMPoint& aCaretPoint,
    const WSRunScanner& aWSRunScannerAtCaret) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aHRElement.IsHTMLElement(nsGkAtoms::hr));
  MOZ_ASSERT(&aHRElement != aWSRunScannerAtCaret.GetEditingHost());

  Result<bool, nsresult> canDeleteHRElement = ShouldDeleteHRElement(
      aHTMLEditor, aDirectionAndAmount, aHRElement, aCaretPoint);
  if (MOZ_UNLIKELY(canDeleteHRElement.isErr())) {
    NS_WARNING("AutoDeleteRangesHandler::ShouldDeleteHRElement() failed");
    return canDeleteHRElement.propagateErr();
  }
  if (canDeleteHRElement.inspect()) {
    Result<EditActionResult, nsresult> result = HandleDeleteAtomicContent(
        aHTMLEditor, aHRElement, aCaretPoint, aWSRunScannerAtCaret);
    NS_WARNING_ASSERTION(
        result.isOk(),
        "AutoDeleteRangesHandler::HandleDeleteAtomicContent() failed");
    return result;
  }

  // Go to the position after the <hr>, but to the end of the <hr> line
  // by setting the interline position to left.
  EditorDOMPoint atNextOfHRElement(EditorDOMPoint::After(aHRElement));
  NS_WARNING_ASSERTION(atNextOfHRElement.IsSet(),
                       "Failed to set after <hr> element");

  {
    AutoEditorDOMPointChildInvalidator lockOffset(atNextOfHRElement);

    nsresult rv = aHTMLEditor.CollapseSelectionTo(atNextOfHRElement);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionTo() failed, but ignored");
  }

  DebugOnly<nsresult> rvIgnored =
      aHTMLEditor.SelectionRef().SetInterlinePosition(
          InterlinePosition::EndOfLine);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                       "Selection::SetInterlinePosition(InterlinePosition::"
                       "EndOfLine) failed, but ignored");
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidExplicitlySetInterLine = true;

  // There is one exception to the move only case.  If the <hr> is
  // followed by a <br> we want to delete the <br>.

  WSScanResult forwardScanFromCaretResult =
      aWSRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(aCaretPoint);
  if (MOZ_UNLIKELY(forwardScanFromCaretResult.Failed())) {
    NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom() failed");
    return Err(NS_ERROR_FAILURE);
  }
  if (!forwardScanFromCaretResult.ReachedBRElement()) {
    return EditActionResult::HandledResult();
  }

  // Delete the <br>
  nsresult rv =
      WhiteSpaceVisibilityKeeper::DeleteContentNodeAndJoinTextNodesAroundIt(
          aHTMLEditor,
          MOZ_KnownLive(*forwardScanFromCaretResult.BRElementPtr()),
          aCaretPoint);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::DeleteContentNodeAndJoinTextNodesAroundIt("
        ") failed");
    return Err(rv);
  }
  return EditActionResult::HandledResult();
}

// static
nsIContent* HTMLEditor::AutoDeleteRangesHandler::GetAtomicContentToDelete(
    nsIEditor::EDirection aDirectionAndAmount,
    const WSRunScanner& aWSRunScannerAtCaret,
    const WSScanResult& aScanFromCaretPointResult) {
  MOZ_ASSERT(aScanFromCaretPointResult.GetContent());

  if (!aScanFromCaretPointResult.ReachedSpecialContent()) {
    return aScanFromCaretPointResult.GetContent();
  }

  if (!aScanFromCaretPointResult.GetContent()->IsText() ||
      HTMLEditUtils::IsRemovableNode(*aScanFromCaretPointResult.GetContent())) {
    return aScanFromCaretPointResult.GetContent();
  }

  // aScanFromCaretPointResult is non-removable text node.
  // Since we try removing atomic content, we look for removable node from
  // scanned point that is non-removable text.
  nsIContent* removableRoot = aScanFromCaretPointResult.GetContent();
  while (removableRoot && !HTMLEditUtils::IsRemovableNode(*removableRoot)) {
    removableRoot = removableRoot->GetParent();
  }

  if (removableRoot) {
    return removableRoot;
  }

  // Not found better content. This content may not be removable.
  return aScanFromCaretPointResult.GetContent();
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteAtomicContent(
    Element* aEditingHost, const nsIContent& aAtomicContent,
    AutoRangeArray& aRangesToDelete) const {
  EditorDOMRange rangeToDelete =
      WSRunScanner::GetRangesForDeletingAtomicContent(aEditingHost,
                                                      aAtomicContent);
  if (!rangeToDelete.IsPositioned()) {
    NS_WARNING("WSRunScanner::GetRangeForDeleteAContentNode() failed");
    return NS_ERROR_FAILURE;
  }
  nsresult rv = aRangesToDelete.SetStartAndEnd(rangeToDelete.StartRef(),
                                               rangeToDelete.EndRef());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoRangeArray::SetStartAndEnd() failed");
  return rv;
}

Result<EditActionResult, nsresult>
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteAtomicContent(
    HTMLEditor& aHTMLEditor, nsIContent& aAtomicContent,
    const EditorDOMPoint& aCaretPoint,
    const WSRunScanner& aWSRunScannerAtCaret) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!HTMLEditUtils::IsInvisibleBRElement(aAtomicContent));
  MOZ_ASSERT(&aAtomicContent != aWSRunScannerAtCaret.GetEditingHost());

  nsresult rv =
      WhiteSpaceVisibilityKeeper::DeleteContentNodeAndJoinTextNodesAroundIt(
          aHTMLEditor, aAtomicContent, aCaretPoint);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::DeleteContentNodeAndJoinTextNodesAroundIt("
        ") failed");
    return Err(rv);
  }

  const auto newCaretPosition =
      aHTMLEditor.GetFirstSelectionStartPoint<EditorDOMPoint>();
  if (MOZ_UNLIKELY(!newCaretPosition.IsSet())) {
    NS_WARNING("There was no selection range");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  rv = aHTMLEditor.InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
      newCaretPosition);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary()"
        " failed");
    return Err(rv);
  }
  return EditActionResult::HandledResult();
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    PrepareToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount, Element& aOtherBlockElement,
        const EditorDOMPoint& aCaretPoint,
        const WSRunScanner& aWSRunScannerAtCaret) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());

  mMode = Mode::JoinOtherBlock;

  // Make sure it's not a table element.  If so, cancel the operation
  // (translation: users cannot backspace or delete across table cells)
  if (HTMLEditUtils::IsAnyTableElement(&aOtherBlockElement)) {
    return false;
  }

  // First find the adjacent node in the block
  if (aDirectionAndAmount == nsIEditor::ePrevious) {
    mLeafContentInOtherBlock = HTMLEditUtils::GetLastLeafContent(
        aOtherBlockElement, {LeafNodeType::OnlyEditableLeafNode},
        &aOtherBlockElement);
    mLeftContent = mLeafContentInOtherBlock;
    mRightContent = aCaretPoint.GetContainerAs<nsIContent>();
  } else {
    mLeafContentInOtherBlock = HTMLEditUtils::GetFirstLeafContent(
        aOtherBlockElement, {LeafNodeType::OnlyEditableLeafNode},
        &aOtherBlockElement);
    mLeftContent = aCaretPoint.GetContainerAs<nsIContent>();
    mRightContent = mLeafContentInOtherBlock;
  }

  // Next to a block.  See if we are between the block and a `<br>`.
  // If so, we really want to delete the `<br>`.  Else join content at
  // selection to the block.
  WSScanResult scanFromCaretResult =
      aDirectionAndAmount == nsIEditor::eNext
          ? aWSRunScannerAtCaret.ScanPreviousVisibleNodeOrBlockBoundaryFrom(
                aCaretPoint)
          : aWSRunScannerAtCaret.ScanNextVisibleNodeOrBlockBoundaryFrom(
                aCaretPoint);
  // If we found a `<br>` element, we need to delete it instead of joining the
  // contents.
  if (scanFromCaretResult.ReachedBRElement()) {
    mBRElement = scanFromCaretResult.BRElementPtr();
    mMode = Mode::DeleteBRElement;
    return true;
  }

  return mLeftContent && mRightContent;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteBRElement(AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(mBRElement);
  // XXX Why don't we scan invisible leading white-spaces which follows the
  //     `<br>` element?
  nsresult rv = aRangesToDelete.SelectNode(*mBRElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::SelectNode() failed");
  return rv;
}

Result<EditActionResult, nsresult>
HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::DeleteBRElement(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    const EditorDOMPoint& aCaretPoint) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());
  MOZ_ASSERT(mBRElement);

  // If we found a `<br>` element, we should delete it instead of joining the
  // contents.
  nsresult rv =
      aHTMLEditor.DeleteNodeWithTransaction(MOZ_KnownLive(*mBRElement));
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
    return Err(rv);
  }

  if (mLeftContent && mRightContent &&
      HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mLeftContent) !=
          HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mRightContent)) {
    return EditActionResult::HandledResult();
  }

  // Put selection at edge of block and we are done.
  if (NS_WARN_IF(!mLeafContentInOtherBlock)) {
    // XXX This must be odd case.  The other block can be empty.
    return Err(NS_ERROR_FAILURE);
  }
  EditorRawDOMPoint newCaretPosition =
      HTMLEditUtils::GetGoodCaretPointFor<EditorRawDOMPoint>(
          *mLeafContentInOtherBlock, aDirectionAndAmount);
  if (MOZ_UNLIKELY(!newCaretPosition.IsSet())) {
    NS_WARNING("HTMLEditUtils::GetGoodCaretPointFor() failed");
    return Err(NS_ERROR_FAILURE);
  }
  rv = aHTMLEditor.CollapseSelectionTo(newCaretPosition);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed, but ignored");
  return EditActionResult::HandledResult();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteAtOtherBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        const EditorDOMPoint& aCaretPoint, AutoRangeArray& aRangesToDelete,
        const Element& aEditingHost) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  if (HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mLeftContent) !=
      HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mRightContent)) {
    if (!mDeleteRangesHandlerConst.CanFallbackToDeleteRangesWithTransaction(
            aRangesToDelete)) {
      nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "AutoRangeArray::Collapse() failed");
      return rv;
    }
    nsresult rv = mDeleteRangesHandlerConst
                      .FallbackToComputeRangesToDeleteRangesWithTransaction(
                          aHTMLEditor, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::"
        "FallbackToComputeRangesToDeleteRangesWithTransaction() failed");
    return rv;
  }

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem =
      joiner.Prepare(aHTMLEditor, aEditingHost);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return canJoinThem.unwrapErr();
  }
  if (canJoinThem.inspect() && joiner.CanJoinBlocks() &&
      !joiner.ShouldDeleteLeafContentInstead()) {
    nsresult rv =
        joiner.ComputeRangesToDelete(aHTMLEditor, aCaretPoint, aRangesToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoInclusiveAncestorBlockElementsJoiner::ComputeRangesToDelete() "
        "failed");
    return rv;
  }

  // If AutoInclusiveAncestorBlockElementsJoiner didn't handle it and it's not
  // canceled, user may want to modify the start leaf node or the last leaf
  // node of the block.
  if (mLeafContentInOtherBlock == aCaretPoint.GetContainer()) {
    return NS_OK;
  }

  AutoHideSelectionChanges hideSelectionChanges(aHTMLEditor.SelectionRef());

  // If it's ignored, it didn't modify the DOM tree.  In this case, user must
  // want to delete nearest leaf node in the other block element.
  // TODO: We need to consider this before calling ComputeRangesToDelete() for
  //       computing the deleting range.
  EditorRawDOMPoint newCaretPoint =
      aDirectionAndAmount == nsIEditor::ePrevious
          ? EditorRawDOMPoint::AtEndOf(*mLeafContentInOtherBlock)
          : EditorRawDOMPoint(mLeafContentInOtherBlock, 0);
  // If new caret position is same as current caret position, we can do
  // nothing anymore.
  if (aRangesToDelete.IsCollapsed() &&
      aRangesToDelete.FocusRef() == newCaretPoint.ToRawRangeBoundary()) {
    return NS_OK;
  }
  // TODO: Stop modifying the `Selection` for computing the targer ranges.
  nsresult rv = aHTMLEditor.CollapseSelectionTo(newCaretPoint);
  if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
    NS_WARNING(
        "EditorBase::CollapseSelectionTo() caused destroying the editor");
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed");
  if (NS_SUCCEEDED(rv)) {
    aRangesToDelete.Initialize(aHTMLEditor.SelectionRef());
    AutoDeleteRangesHandler anotherHandler(mDeleteRangesHandlerConst);
    rv = anotherHandler.ComputeRangesToDelete(aHTMLEditor, aDirectionAndAmount,
                                              aRangesToDelete, aEditingHost);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "Recursive AutoDeleteRangesHandler::ComputeRangesToDelete() failed");
  }
  // Restore selection.
  nsresult rvCollapsingSelectionTo =
      aHTMLEditor.CollapseSelectionTo(aCaretPoint);
  if (MOZ_UNLIKELY(rvCollapsingSelectionTo == NS_ERROR_EDITOR_DESTROYED)) {
    NS_WARNING(
        "EditorBase::CollapseSelectionTo() caused destroying the editor");
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rvCollapsingSelectionTo),
      "EditorBase::CollapseSelectionTo() failed to restore caret position");
  return NS_SUCCEEDED(rv) && NS_SUCCEEDED(rvCollapsingSelectionTo)
             ? NS_OK
             : NS_ERROR_FAILURE;
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::HandleDeleteAtOtherBlockBoundary(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        const EditorDOMPoint& aCaretPoint, AutoRangeArray& aRangesToDelete,
        const Element& aEditingHost) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(aCaretPoint.IsSetAndValid());
  MOZ_ASSERT(mDeleteRangesHandler);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  if (HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mLeftContent) !=
      HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mRightContent)) {
    // If we have not deleted `<br>` element and are not called recursively,
    // we should call `DeleteRangesWithTransaction()` here.
    if (!mDeleteRangesHandler->CanFallbackToDeleteRangesWithTransaction(
            aRangesToDelete)) {
      return EditActionResult::IgnoredResult();
    }
    Result<EditActionResult, nsresult> result =
        mDeleteRangesHandler->FallbackToDeleteRangesWithTransaction(
            aHTMLEditor, aRangesToDelete);
    NS_WARNING_ASSERTION(
        result.isOk(),
        "AutoDeleteRangesHandler::FallbackToDeleteRangesWithTransaction() "
        "failed to delete leaf content in the block");
    return result;
  }

  // Else we are joining content to block
  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem =
      joiner.Prepare(aHTMLEditor, aEditingHost);
  if (MOZ_UNLIKELY(canJoinThem.isErr())) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return canJoinThem.propagateErr();
  }

  if (!canJoinThem.inspect()) {
    nsresult rv = aHTMLEditor.CollapseSelectionTo(aCaretPoint);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionTo() failed, but ignored");
    return EditActionResult::CanceledResult();
  }

  auto result = EditActionResult::IgnoredResult();
  EditorDOMPoint pointToPutCaret(aCaretPoint);
  if (joiner.CanJoinBlocks()) {
    {
      AutoTrackDOMPoint tracker(aHTMLEditor.RangeUpdaterRef(),
                                &pointToPutCaret);
      Result<EditActionResult, nsresult> joinResult =
          joiner.Run(aHTMLEditor, aEditingHost);
      if (MOZ_UNLIKELY(joinResult.isErr())) {
        NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Run() failed");
        return joinResult;
      }
      result |= joinResult.unwrap();
#ifdef DEBUG
      if (joiner.ShouldDeleteLeafContentInstead()) {
        NS_ASSERTION(
            result.Ignored(),
            "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
            "returning ignored, but returned not ignored");
      } else {
        NS_ASSERTION(
            !result.Ignored(),
            "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
            "returning handled, but returned ignored");
      }
#endif  // #ifdef DEBUG
    }

    // If AutoInclusiveAncestorBlockElementsJoiner didn't handle it and it's not
    // canceled, user may want to modify the start leaf node or the last leaf
    // node of the block.
    if (result.Ignored() &&
        mLeafContentInOtherBlock != aCaretPoint.GetContainer()) {
      // If it's ignored, it didn't modify the DOM tree.  In this case, user
      // must want to delete nearest leaf node in the other block element.
      // TODO: We need to consider this before calling Run() for computing the
      //       deleting range.
      EditorRawDOMPoint newCaretPoint =
          aDirectionAndAmount == nsIEditor::ePrevious
              ? EditorRawDOMPoint::AtEndOf(*mLeafContentInOtherBlock)
              : EditorRawDOMPoint(mLeafContentInOtherBlock, 0);
      // If new caret position is same as current caret position, we can do
      // nothing anymore.
      if (aRangesToDelete.IsCollapsed() &&
          aRangesToDelete.FocusRef() == newCaretPoint.ToRawRangeBoundary()) {
        return EditActionResult::CanceledResult();
      }
      nsresult rv = aHTMLEditor.CollapseSelectionTo(newCaretPoint);
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::CollapseSelectionTo() failed");
        return Err(rv);
      }
      AutoRangeArray rangesToDelete(aHTMLEditor.SelectionRef());
      AutoDeleteRangesHandler anotherHandler(mDeleteRangesHandler);
      Result<EditActionResult, nsresult> fallbackResult =
          anotherHandler.Run(aHTMLEditor, aDirectionAndAmount, aStripWrappers,
                             rangesToDelete, aEditingHost);
      if (MOZ_UNLIKELY(fallbackResult.isErr())) {
        NS_WARNING("Recursive AutoDeleteRangesHandler::Run() failed");
        return fallbackResult;
      }
      result |= fallbackResult.unwrap();
      return result;
    }
  } else {
    result.MarkAsHandled();
  }

  // Otherwise, we must have deleted the selection as user expected.
  nsresult rv = aHTMLEditor.CollapseSelectionTo(pointToPutCaret);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed, but ignored");
  return result;
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    PrepareToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        Element& aCurrentBlockElement, const EditorDOMPoint& aCaretPoint) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  // At edge of our block.  Look beside it and see if we can join to an
  // adjacent block
  mMode = Mode::JoinCurrentBlock;

  // Don't break the basic structure of the HTML document.
  if (aCurrentBlockElement.IsAnyOfHTMLElements(nsGkAtoms::html, nsGkAtoms::head,
                                               nsGkAtoms::body)) {
    return false;
  }

  // Make sure it's not a table element.  If so, cancel the operation
  // (translation: users cannot backspace or delete across table cells)
  if (HTMLEditUtils::IsAnyTableElement(&aCurrentBlockElement)) {
    return false;
  }

  Element* editingHost = aHTMLEditor.ComputeEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return false;
  }

  auto ScanJoinTarget = [&]() -> nsIContent* {
    nsIContent* targetContent =
        aDirectionAndAmount == nsIEditor::ePrevious
            ? HTMLEditUtils::GetPreviousContent(
                  aCurrentBlockElement, {WalkTreeOption::IgnoreNonEditableNode},
                  editingHost)
            : HTMLEditUtils::GetNextContent(
                  aCurrentBlockElement, {WalkTreeOption::IgnoreNonEditableNode},
                  editingHost);
    // If found content is an invisible text node, let's scan visible things.
    auto IsIgnorableDataNode = [](nsIContent* aContent) {
      return aContent && HTMLEditUtils::IsRemovableNode(*aContent) &&
             ((aContent->IsText() &&
               aContent->AsText()->TextIsOnlyWhitespace() &&
               !HTMLEditUtils::IsVisibleTextNode(*aContent->AsText())) ||
              (aContent->IsCharacterData() && !aContent->IsText()));
    };
    if (!IsIgnorableDataNode(targetContent)) {
      return targetContent;
    }
    MOZ_ASSERT(mSkippedInvisibleContents.IsEmpty());
    for (nsIContent* adjacentContent =
             aDirectionAndAmount == nsIEditor::ePrevious
                 ? HTMLEditUtils::GetPreviousContent(
                       *targetContent, {WalkTreeOption::StopAtBlockBoundary},
                       editingHost)
                 : HTMLEditUtils::GetNextContent(
                       *targetContent, {WalkTreeOption::StopAtBlockBoundary},
                       editingHost);
         adjacentContent;
         adjacentContent =
             aDirectionAndAmount == nsIEditor::ePrevious
                 ? HTMLEditUtils::GetPreviousContent(
                       *adjacentContent, {WalkTreeOption::StopAtBlockBoundary},
                       editingHost)
                 : HTMLEditUtils::GetNextContent(
                       *adjacentContent, {WalkTreeOption::StopAtBlockBoundary},
                       editingHost)) {
      // If non-editable element is found, we should not skip it to avoid
      // joining too far nodes.
      if (!HTMLEditUtils::IsSimplyEditableNode(*adjacentContent)) {
        break;
      }
      // If block element is found, we should join last leaf content in it.
      if (HTMLEditUtils::IsBlockElement(*adjacentContent)) {
        nsIContent* leafContent =
            aDirectionAndAmount == nsIEditor::ePrevious
                ? HTMLEditUtils::GetLastLeafContent(
                      *adjacentContent, {LeafNodeType::OnlyEditableLeafNode})
                : HTMLEditUtils::GetFirstLeafContent(
                      *adjacentContent, {LeafNodeType::OnlyEditableLeafNode});
        mSkippedInvisibleContents.AppendElement(*targetContent);
        return leafContent ? leafContent : adjacentContent;
      }
      // Only when the found node is an invisible text node or a non-text data
      // node, we should keep scanning.
      if (IsIgnorableDataNode(adjacentContent)) {
        mSkippedInvisibleContents.AppendElement(*targetContent);
        targetContent = adjacentContent;
        continue;
      }
      // Otherwise, we find a visible things. We should join with last found
      // invisible text node.
      break;
    }
    return targetContent;
  };

  if (aDirectionAndAmount == nsIEditor::ePrevious) {
    mLeftContent = ScanJoinTarget();
    mRightContent = aCaretPoint.GetContainerAs<nsIContent>();
  } else {
    mRightContent = ScanJoinTarget();
    mLeftContent = aCaretPoint.GetContainerAs<nsIContent>();
  }

  // Nothing to join
  if (!mLeftContent || !mRightContent) {
    return false;
  }

  // Don't cross table boundaries.
  return HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mLeftContent) ==
         HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*mRightContent);
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteAtCurrentBlockBoundary(
        const HTMLEditor& aHTMLEditor, const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete, const Element& aEditingHost) const {
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem =
      joiner.Prepare(aHTMLEditor, aEditingHost);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return canJoinThem.unwrapErr();
  }
  if (canJoinThem.inspect()) {
    nsresult rv =
        joiner.ComputeRangesToDelete(aHTMLEditor, aCaretPoint, aRangesToDelete);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "AutoInclusiveAncestorBlockElementsJoiner::"
                         "ComputeRangesToDelete() failed");
    return rv;
  }

  // In this case, nothing will be deleted so that the affected range should
  // be collapsed.
  nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
  return rv;
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::HandleDeleteAtCurrentBlockBoundary(
        HTMLEditor& aHTMLEditor, const EditorDOMPoint& aCaretPoint,
        const Element& aEditingHost) {
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mRightContent);

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem =
      joiner.Prepare(aHTMLEditor, aEditingHost);
  if (MOZ_UNLIKELY(canJoinThem.isErr())) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return Err(canJoinThem.unwrapErr());
  }

  if (!canJoinThem.inspect()) {
    nsresult rv = aHTMLEditor.CollapseSelectionTo(aCaretPoint);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionTo() failed, but ignored");
    return EditActionResult::CanceledResult();
  }

  EditActionResult result = EditActionResult::IgnoredResult();
  EditorDOMPoint pointToPutCaret(aCaretPoint);
  if (joiner.CanJoinBlocks()) {
    AutoTrackDOMPoint tracker(aHTMLEditor.RangeUpdaterRef(), &pointToPutCaret);
    Result<EditActionResult, nsresult> joinResult =
        joiner.Run(aHTMLEditor, aEditingHost);
    if (MOZ_UNLIKELY(joinResult.isErr())) {
      NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Run() failed");
      return joinResult;
    }
    result |= joinResult.unwrap();
#ifdef DEBUG
    if (joiner.ShouldDeleteLeafContentInstead()) {
      NS_ASSERTION(result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "retruning ignored, but returned not ignored");
    } else {
      NS_ASSERTION(!result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "retruning handled, but returned ignored");
    }
#endif  // #ifdef DEBUG

    // Cleaning up invisible nodes which are skipped at scanning mLeftContent or
    // mRightContent.
    for (const OwningNonNull<nsIContent>& content : mSkippedInvisibleContents) {
      nsresult rv =
          aHTMLEditor.DeleteNodeWithTransaction(MOZ_KnownLive(content));
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return Err(rv);
      }
    }
    mSkippedInvisibleContents.Clear();
  }
  // This should claim that trying to join the block means that
  // this handles the action because the caller shouldn't do anything
  // anymore in this case.
  result.MarkAsHandled();

  nsresult rv = aHTMLEditor.CollapseSelectionTo(pointToPutCaret);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed, but ignored");
  return result;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteNonCollapsedRanges(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete,
    AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed,
    const Element& aEditingHost) const {
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());

  if (NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->StartRef().IsSet()) ||
      NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->EndRef().IsSet())) {
    return NS_ERROR_FAILURE;
  }

  if (aRangesToDelete.Ranges().Length() == 1) {
    nsFrameSelection* frameSelection =
        aHTMLEditor.SelectionRef().GetFrameSelection();
    if (NS_WARN_IF(!frameSelection)) {
      return NS_ERROR_FAILURE;
    }
    Result<EditorRawDOMRange, nsresult> result = ExtendOrShrinkRangeToDelete(
        aHTMLEditor, frameSelection,
        EditorRawDOMRange(aRangesToDelete.FirstRangeRef()));
    if (MOZ_UNLIKELY(result.isErr())) {
      NS_WARNING(
          "AutoDeleteRangesHandler::ExtendOrShrinkRangeToDelete() failed");
      return NS_ERROR_FAILURE;
    }
    EditorRawDOMRange newRange(result.unwrap());
    if (MOZ_UNLIKELY(NS_FAILED(aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
            newRange.StartRef().ToRawRangeBoundary(),
            newRange.EndRef().ToRawRangeBoundary())))) {
      NS_WARNING("nsRange::SetStartAndEnd() failed");
      return NS_ERROR_FAILURE;
    }
    if (MOZ_UNLIKELY(
            NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->IsPositioned()))) {
      return NS_ERROR_FAILURE;
    }
    if (NS_WARN_IF(aRangesToDelete.FirstRangeRef()->Collapsed())) {
      return NS_OK;  // Hmm, there is nothing to delete...?
    }
  }

  if (!aHTMLEditor.IsInPlaintextMode()) {
    EditorDOMRange firstRange(aRangesToDelete.FirstRangeRef());
    EditorDOMRange extendedRange =
        WSRunScanner::GetRangeContainingInvisibleWhiteSpacesAtRangeBoundaries(
            aHTMLEditor.ComputeEditingHost(),
            EditorDOMRange(aRangesToDelete.FirstRangeRef()));
    if (firstRange != extendedRange) {
      nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
          extendedRange.StartRef().ToRawRangeBoundary(),
          extendedRange.EndRef().ToRawRangeBoundary());
      if (NS_FAILED(rv)) {
        NS_WARNING("nsRange::SetStartAndEnd() failed");
        return NS_ERROR_FAILURE;
      }
    }
  }

  if (aRangesToDelete.FirstRangeRef()->GetStartContainer() ==
      aRangesToDelete.FirstRangeRef()->GetEndContainer()) {
    if (!aRangesToDelete.FirstRangeRef()->Collapsed()) {
      nsresult rv = ComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "AutoDeleteRangesHandler::ComputeRangesToDeleteRangesWithTransaction("
          ") failed");
      return rv;
    }
    // `DeleteUnnecessaryNodesAndCollapseSelection()` may delete parent
    // elements, but it does not affect computing target ranges.  Therefore,
    // we don't need to touch aRangesToDelete in this case.
    return NS_OK;
  }

  Element* startCiteNode = aHTMLEditor.GetMostDistantAncestorMailCiteElement(
      *aRangesToDelete.FirstRangeRef()->GetStartContainer());
  Element* endCiteNode = aHTMLEditor.GetMostDistantAncestorMailCiteElement(
      *aRangesToDelete.FirstRangeRef()->GetEndContainer());

  if (startCiteNode && !endCiteNode) {
    aDirectionAndAmount = nsIEditor::eNext;
  } else if (!startCiteNode && endCiteNode) {
    aDirectionAndAmount = nsIEditor::ePrevious;
  }

  AutoBlockElementsJoiner joiner(*this);
  if (!joiner.PrepareToDeleteNonCollapsedRanges(aHTMLEditor, aRangesToDelete)) {
    return NS_ERROR_FAILURE;
  }
  nsresult rv = joiner.ComputeRangesToDelete(
      aHTMLEditor, aDirectionAndAmount, aRangesToDelete, aSelectionWasCollapsed,
      aEditingHost);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoBlockElementsJoiner::ComputeRangesToDelete() failed");
  return rv;
}

Result<EditActionResult, nsresult>
HTMLEditor::AutoDeleteRangesHandler::HandleDeleteNonCollapsedRanges(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    nsIEditor::EStripWrappers aStripWrappers, AutoRangeArray& aRangesToDelete,
    SelectionWasCollapsed aSelectionWasCollapsed, const Element& aEditingHost) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());

  if (NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->StartRef().IsSet()) ||
      NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->EndRef().IsSet())) {
    return Err(NS_ERROR_FAILURE);
  }

  MOZ_ASSERT_IF(aRangesToDelete.Ranges().Length() == 1,
                aRangesToDelete.IsFirstRangeEditable(aEditingHost));

  // Else we have a non-collapsed selection.  First adjust the selection.
  // XXX Why do we extend selection only when there is only one range?
  if (aRangesToDelete.Ranges().Length() == 1) {
    nsFrameSelection* frameSelection =
        aHTMLEditor.SelectionRef().GetFrameSelection();
    if (NS_WARN_IF(!frameSelection)) {
      return Err(NS_ERROR_FAILURE);
    }
    Result<EditorRawDOMRange, nsresult> result = ExtendOrShrinkRangeToDelete(
        aHTMLEditor, frameSelection,
        EditorRawDOMRange(aRangesToDelete.FirstRangeRef()));
    if (MOZ_UNLIKELY(result.isErr())) {
      NS_WARNING(
          "AutoDeleteRangesHandler::ExtendOrShrinkRangeToDelete() failed");
      return Err(NS_ERROR_FAILURE);
    }
    EditorRawDOMRange newRange(result.unwrap());
    if (NS_FAILED(aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
            newRange.StartRef().ToRawRangeBoundary(),
            newRange.EndRef().ToRawRangeBoundary()))) {
      NS_WARNING("nsRange::SetStartAndEnd() failed");
      return Err(NS_ERROR_FAILURE);
    }
    if (NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->IsPositioned())) {
      return Err(NS_ERROR_FAILURE);
    }
    if (NS_WARN_IF(aRangesToDelete.FirstRangeRef()->Collapsed())) {
      // Hmm, there is nothing to delete...?
      return EditActionResult::HandledResult();
    }
    MOZ_ASSERT(aRangesToDelete.IsFirstRangeEditable(aEditingHost));
  }

  // Remember that we did a ranged delete for the benefit of AfterEditInner().
  aHTMLEditor.TopLevelEditSubActionDataRef().mDidDeleteNonCollapsedRange = true;

  // Figure out if the endpoints are in nodes that can be merged.  Adjust
  // surrounding white-space in preparation to delete selection.
  if (!aHTMLEditor.IsInPlaintextMode()) {
    {
      AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
      AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                          &aRangesToDelete.FirstRangeRef());
      nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRange(
          aHTMLEditor, EditorDOMRange(aRangesToDelete.FirstRangeRef()));
      if (NS_FAILED(rv)) {
        NS_WARNING("WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() failed");
        return Err(rv);
      }
    }
    if (NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->IsPositioned()) ||
        (aHTMLEditor.MayHaveMutationEventListeners() &&
         NS_WARN_IF(!aRangesToDelete.IsFirstRangeEditable(aEditingHost)))) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() made the first "
          "range invalid");
      return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
    }
  }

  // XXX This is odd.  We do we simply use `DeleteRangesWithTransaction()`
  //     only when **first** range is in same container?
  if (aRangesToDelete.FirstRangeRef()->GetStartContainer() ==
      aRangesToDelete.FirstRangeRef()->GetEndContainer()) {
    // Because of previous DOM tree changes, the range may be collapsed.
    // If we've already removed all contents in the range, we shouldn't
    // delete anything around the caret.
    if (!aRangesToDelete.FirstRangeRef()->Collapsed()) {
      {
        AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                            &aRangesToDelete.FirstRangeRef());
        nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
            aDirectionAndAmount, aStripWrappers, aRangesToDelete);
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::DeleteRangesWithTransaction() failed");
          return Err(rv);
        }
      }
      if (NS_WARN_IF(!aRangesToDelete.FirstRangeRef()->IsPositioned()) ||
          (aHTMLEditor.MayHaveMutationEventListeners(
               NS_EVENT_BITS_MUTATION_NODEREMOVED |
               NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
               NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED) &&
           NS_WARN_IF(!aRangesToDelete.IsFirstRangeEditable(aEditingHost)))) {
        NS_WARNING(
            "EditorBase::DeleteRangesWithTransaction() made the first range "
            "invalid");
        return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
    }
    // However, even if the range is removed, we may need to clean up the
    // containers which become empty.
    nsresult rv = DeleteUnnecessaryNodesAndCollapseSelection(
        aHTMLEditor, aDirectionAndAmount,
        EditorDOMPoint(aRangesToDelete.FirstRangeRef()->StartRef()),
        EditorDOMPoint(aRangesToDelete.FirstRangeRef()->EndRef()));
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoDeleteRangesHandler::DeleteUnnecessaryNodesAndCollapseSelection("
          ") failed");
      return Err(rv);
    }
    return EditActionResult::HandledResult();
  }

  if (NS_WARN_IF(
          !aRangesToDelete.FirstRangeRef()->GetStartContainer()->IsContent()) ||
      NS_WARN_IF(
          !aRangesToDelete.FirstRangeRef()->GetEndContainer()->IsContent())) {
    return Err(NS_ERROR_FAILURE);
  }

  // Figure out mailcite ancestors
  RefPtr<Element> startCiteNode =
      aHTMLEditor.GetMostDistantAncestorMailCiteElement(
          *aRangesToDelete.FirstRangeRef()->GetStartContainer());
  RefPtr<Element> endCiteNode =
      aHTMLEditor.GetMostDistantAncestorMailCiteElement(
          *aRangesToDelete.FirstRangeRef()->GetEndContainer());

  // If we only have a mailcite at one of the two endpoints, set the
  // directionality of the deletion so that the selection will end up
  // outside the mailcite.
  if (startCiteNode && !endCiteNode) {
    aDirectionAndAmount = nsIEditor::eNext;
  } else if (!startCiteNode && endCiteNode) {
    aDirectionAndAmount = nsIEditor::ePrevious;
  }

  AutoBlockElementsJoiner joiner(*this);
  if (!joiner.PrepareToDeleteNonCollapsedRanges(aHTMLEditor, aRangesToDelete)) {
    return Err(NS_ERROR_FAILURE);
  }
  Result<EditActionResult, nsresult> result =
      joiner.Run(aHTMLEditor, aDirectionAndAmount, aStripWrappers,
                 aRangesToDelete, aSelectionWasCollapsed, aEditingHost);
  NS_WARNING_ASSERTION(result.isOk(), "AutoBlockElementsJoiner::Run() failed");
  return result;
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    PrepareToDeleteNonCollapsedRanges(const HTMLEditor& aHTMLEditor,
                                      const AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());

  mLeftContent = HTMLEditUtils::GetInclusiveAncestorElement(
      *aRangesToDelete.FirstRangeRef()->GetStartContainer()->AsContent(),
      HTMLEditUtils::ClosestEditableBlockElement);
  mRightContent = HTMLEditUtils::GetInclusiveAncestorElement(
      *aRangesToDelete.FirstRangeRef()->GetEndContainer()->AsContent(),
      HTMLEditUtils::ClosestEditableBlockElement);
  // Note that mLeftContent and/or mRightContent can be nullptr if editing host
  // is an inline element.  If both editable ancestor block is exactly same
  // one or one reaches an inline editing host, we can just delete the content
  // in ranges.
  if (mLeftContent == mRightContent || !mLeftContent || !mRightContent) {
    MOZ_ASSERT_IF(!mLeftContent || !mRightContent,
                  aRangesToDelete.FirstRangeRef()
                          ->GetStartContainer()
                          ->AsContent()
                          ->GetEditingHost() == aRangesToDelete.FirstRangeRef()
                                                    ->GetEndContainer()
                                                    ->AsContent()
                                                    ->GetEditingHost());
    mMode = Mode::DeleteContentInRanges;
    return true;
  }

  // If left block and right block are adjuscent siblings and they are same
  // type of elements, we can merge them after deleting the selected contents.
  // MOOSE: this could conceivably screw up a table.. fix me.
  if (mLeftContent->GetParentNode() == mRightContent->GetParentNode() &&
      HTMLEditUtils::CanContentsBeJoined(
          *mLeftContent, *mRightContent,
          aHTMLEditor.IsCSSEnabled() ? StyleDifference::CompareIfSpanElements
                                     : StyleDifference::Ignore) &&
      // XXX What's special about these three types of block?
      (mLeftContent->IsHTMLElement(nsGkAtoms::p) ||
       HTMLEditUtils::IsListItem(mLeftContent) ||
       HTMLEditUtils::IsHeader(*mLeftContent))) {
    mMode = Mode::JoinBlocksInSameParent;
    return true;
  }

  mMode = Mode::DeleteNonCollapsedRanges;
  return true;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteContentInRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::DeleteContentInRanges);
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->AsContent()
                 ->GetEditingHost());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->AsContent()
                 ->GetEditingHost() == aRangesToDelete.FirstRangeRef()
                                           ->GetEndContainer()
                                           ->AsContent()
                                           ->GetEditingHost());
  MOZ_ASSERT(!mLeftContent == !mRightContent);
  MOZ_ASSERT_IF(mLeftContent, mLeftContent->IsElement());
  MOZ_ASSERT_IF(mLeftContent, aRangesToDelete.FirstRangeRef()
                                  ->GetStartContainer()
                                  ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT_IF(mRightContent, mRightContent->IsElement());
  MOZ_ASSERT_IF(mRightContent, aRangesToDelete.FirstRangeRef()
                                   ->GetEndContainer()
                                   ->IsInclusiveDescendantOf(mRightContent));
  MOZ_ASSERT_IF(!mLeftContent,
                HTMLEditUtils::IsInlineElement(*aRangesToDelete.FirstRangeRef()
                                                    ->GetStartContainer()
                                                    ->AsContent()
                                                    ->GetEditingHost()));

  nsresult rv =
      mDeleteRangesHandlerConst.ComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "ComputeRangesToDeleteRangesWithTransaction() failed");
  return rv;
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::DeleteContentInRanges(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::DeleteContentInRanges);
  MOZ_ASSERT(mDeleteRangesHandler);
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->AsContent()
                 ->GetEditingHost());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->AsContent()
                 ->GetEditingHost() == aRangesToDelete.FirstRangeRef()
                                           ->GetEndContainer()
                                           ->AsContent()
                                           ->GetEditingHost());
  MOZ_ASSERT_IF(mLeftContent, mLeftContent->IsElement());
  MOZ_ASSERT_IF(mLeftContent, aRangesToDelete.FirstRangeRef()
                                  ->GetStartContainer()
                                  ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT_IF(mRightContent, mRightContent->IsElement());
  MOZ_ASSERT_IF(mRightContent, aRangesToDelete.FirstRangeRef()
                                   ->GetEndContainer()
                                   ->IsInclusiveDescendantOf(mRightContent));
  MOZ_ASSERT_IF(!mLeftContent,
                HTMLEditUtils::IsInlineElement(*aRangesToDelete.FirstRangeRef()
                                                    ->GetStartContainer()
                                                    ->AsContent()
                                                    ->GetEditingHost()));

  // XXX This is also odd.  We do we simply use
  //     `DeleteRangesWithTransaction()` only when **first** range is in
  //     same block?
  {
    AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                        &aRangesToDelete.FirstRangeRef());
    nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
        aDirectionAndAmount, aStripWrappers, aRangesToDelete);
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::DeleteRangesWithTransaction() failed, but ignored");
  }
  nsresult rv =
      mDeleteRangesHandler->DeleteUnnecessaryNodesAndCollapseSelection(
          aHTMLEditor, aDirectionAndAmount,
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->StartRef()),
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->EndRef()));
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "AutoDeleteRangesHandler::DeleteUnnecessaryNodesAndCollapseSelection() "
        "failed");
    return Err(rv);
  }
  return EditActionResult::HandledResult();
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToJoinBlockElementsInSameParent(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::JoinBlocksInSameParent);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));
  MOZ_ASSERT(mLeftContent->GetParentNode() == mRightContent->GetParentNode());

  nsresult rv =
      mDeleteRangesHandlerConst.ComputeRangesToDeleteRangesWithTransaction(
          aHTMLEditor, aDirectionAndAmount, aRangesToDelete);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "ComputeRangesToDeleteRangesWithTransaction() failed");
  return rv;
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::JoinBlockElementsInSameParent(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mMode == Mode::JoinBlocksInSameParent);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));
  MOZ_ASSERT(mLeftContent->GetParentNode() == mRightContent->GetParentNode());

  nsresult rv = aHTMLEditor.DeleteRangesWithTransaction(
      aDirectionAndAmount, aStripWrappers, aRangesToDelete);
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteRangesWithTransaction() failed");
    return Err(rv);
  }

  if (NS_WARN_IF(!mLeftContent->GetParentNode()) ||
      NS_WARN_IF(!mRightContent->GetParentNode()) ||
      NS_WARN_IF(mLeftContent->GetParentNode() !=
                 mRightContent->GetParentNode())) {
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  Result<EditorDOMPoint, nsresult> atFirstChildOfTheLastRightNodeOrError =
      JoinNodesDeepWithTransaction(aHTMLEditor, MOZ_KnownLive(*mLeftContent),
                                   MOZ_KnownLive(*mRightContent));
  if (MOZ_UNLIKELY(atFirstChildOfTheLastRightNodeOrError.isErr())) {
    NS_WARNING("HTMLEditor::JoinNodesDeepWithTransaction() failed");
    return atFirstChildOfTheLastRightNodeOrError.propagateErr();
  }
  MOZ_ASSERT(atFirstChildOfTheLastRightNodeOrError.inspect().IsSet());

  rv = aHTMLEditor.CollapseSelectionTo(
      atFirstChildOfTheLastRightNodeOrError.inspect());
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::CollapseSelectionTo() failed");
    return Err(rv);
  }
  return EditActionResult::HandledResult();
}

Result<bool, nsresult>
HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  AutoTArray<OwningNonNull<nsIContent>, 10> arrayOfTopChildren;
  DOMSubtreeIterator iter;
  nsresult rv = iter.Init(aRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("DOMSubtreeIterator::Init() failed");
    return Err(rv);
  }
  iter.AppendAllNodesToArray(arrayOfTopChildren);
  return NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
      aHTMLEditor, arrayOfTopChildren, aSelectionWasCollapsed);
}

Result<bool, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::DeleteNodesEntirelyInRangeButKeepTableStructure(
        HTMLEditor& aHTMLEditor, nsRange& aRange,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  // Build a list of direct child nodes in the range
  AutoTArray<OwningNonNull<nsIContent>, 10> arrayOfTopChildren;
  DOMSubtreeIterator iter;
  nsresult rv = iter.Init(aRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("DOMSubtreeIterator::Init() failed");
    return Err(rv);
  }
  iter.AppendAllNodesToArray(arrayOfTopChildren);

  // Now that we have the list, delete non-table elements
  bool needsToJoinLater =
      NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
          aHTMLEditor, arrayOfTopChildren, aSelectionWasCollapsed);
  for (auto& content : arrayOfTopChildren) {
    // XXX After here, the child contents in the array may have been moved
    //     to somewhere or removed.  We should handle it.
    //
    // MOZ_KnownLive because 'arrayOfTopChildren' is guaranteed to
    // keep it alive.
    //
    // Even with https://bugzilla.mozilla.org/show_bug.cgi?id=1620312 fixed
    // this might need to stay, because 'arrayOfTopChildren' is not const,
    // so it's not obvious how to prove via static analysis that it won't
    // change and release us.
    nsresult rv =
        DeleteContentButKeepTableStructure(aHTMLEditor, MOZ_KnownLive(content));
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoBlockElementsJoiner::DeleteContentButKeepTableStructure() failed, "
        "but ignored");
  }
  return needsToJoinLater;
}

bool HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    NeedsToJoinNodesAfterDeleteNodesEntirelyInRangeButKeepTableStructure(
        const HTMLEditor& aHTMLEditor,
        const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed)
        const {
  // If original selection was collapsed, we need always to join the nodes.
  // XXX Why?
  if (aSelectionWasCollapsed ==
      AutoDeleteRangesHandler::SelectionWasCollapsed::No) {
    return true;
  }
  // If something visible is deleted, no need to join.  Visible means
  // all nodes except non-visible textnodes and breaks.
  if (aArrayOfContents.IsEmpty()) {
    return true;
  }
  for (const OwningNonNull<nsIContent>& content : aArrayOfContents) {
    if (content->IsText()) {
      if (HTMLEditUtils::IsInVisibleTextFrames(aHTMLEditor.GetPresContext(),
                                               *content->AsText())) {
        return false;
      }
      continue;
    }
    // XXX If it's an element node, we should check whether it has visible
    //     frames or not.
    if (!content->IsElement() ||
        HTMLEditUtils::IsEmptyNode(
            *content->AsElement(),
            {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
      continue;
    }
    if (!HTMLEditUtils::IsInvisibleBRElement(*content)) {
      return false;
    }
  }
  return true;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    DeleteTextAtStartAndEndOfRange(HTMLEditor& aHTMLEditor, nsRange& aRange) {
  EditorDOMPoint rangeStart(aRange.StartRef());
  EditorDOMPoint rangeEnd(aRange.EndRef());
  if (rangeStart.IsInTextNode() && !rangeStart.IsEndOfContainer()) {
    // Delete to last character
    OwningNonNull<Text> textNode = *rangeStart.ContainerAs<Text>();
    nsresult rv = aHTMLEditor.DeleteTextWithTransaction(
        textNode, rangeStart.Offset(),
        rangeStart.GetContainer()->Length() - rangeStart.Offset());
    if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
      return rv;
    }
  }
  if (rangeEnd.IsInTextNode() && !rangeEnd.IsStartOfContainer()) {
    // Delete to first character
    OwningNonNull<Text> textNode = *rangeEnd.ContainerAs<Text>();
    nsresult rv =
        aHTMLEditor.DeleteTextWithTransaction(textNode, 0, rangeEnd.Offset());
    if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
      return rv;
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    ComputeRangesToDeleteNonCollapsedRanges(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed,
        const Element& aEditingHost) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));

  for (OwningNonNull<nsRange>& range : aRangesToDelete.Ranges()) {
    Result<bool, nsresult> result =
        ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure(
            aHTMLEditor, range, aSelectionWasCollapsed);
    if (result.isErr()) {
      NS_WARNING(
          "AutoBlockElementsJoiner::"
          "ComputeRangesToDeleteNodesEntirelyInRangeButKeepTableStructure() "
          "failed");
      return result.unwrapErr();
    }
    if (!result.unwrap()) {
      return NS_OK;
    }
  }

  AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                  *mRightContent);
  Result<bool, nsresult> canJoinThem =
      joiner.Prepare(aHTMLEditor, aEditingHost);
  if (canJoinThem.isErr()) {
    NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
    return canJoinThem.unwrapErr();
  }

  if (!canJoinThem.unwrap()) {
    return NS_SUCCESS_DOM_NO_OPERATION;
  }

  if (!joiner.CanJoinBlocks()) {
    return NS_OK;
  }

  nsresult rv = joiner.ComputeRangesToDelete(aHTMLEditor, EditorDOMPoint(),
                                             aRangesToDelete);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "AutoInclusiveAncestorBlockElementsJoiner::ComputeRangesToDelete() "
      "failed");
  return rv;
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::HandleDeleteNonCollapsedRanges(
        HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
        nsIEditor::EStripWrappers aStripWrappers,
        AutoRangeArray& aRangesToDelete,
        AutoDeleteRangesHandler::SelectionWasCollapsed aSelectionWasCollapsed,
        const Element& aEditingHost) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.IsCollapsed());
  MOZ_ASSERT(mDeleteRangesHandler);
  MOZ_ASSERT(mLeftContent);
  MOZ_ASSERT(mLeftContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetStartContainer()
                 ->IsInclusiveDescendantOf(mLeftContent));
  MOZ_ASSERT(mRightContent);
  MOZ_ASSERT(mRightContent->IsElement());
  MOZ_ASSERT(aRangesToDelete.FirstRangeRef()
                 ->GetEndContainer()
                 ->IsInclusiveDescendantOf(mRightContent));

  // Otherwise, delete every nodes in all ranges, then, clean up something.
  EditActionResult result = EditActionResult::IgnoredResult();
  while (true) {
    AutoTrackDOMRange firstRangeTracker(aHTMLEditor.RangeUpdaterRef(),
                                        &aRangesToDelete.FirstRangeRef());

    bool joinInclusiveAncestorBlockElements = true;
    for (auto& range : aRangesToDelete.Ranges()) {
      Result<bool, nsresult> deleteResult =
          DeleteNodesEntirelyInRangeButKeepTableStructure(
              aHTMLEditor, MOZ_KnownLive(range), aSelectionWasCollapsed);
      if (MOZ_UNLIKELY(deleteResult.isErr())) {
        NS_WARNING(
            "AutoBlockElementsJoiner::"
            "DeleteNodesEntirelyInRangeButKeepTableStructure() failed");
        return deleteResult.propagateErr();
      }
      // XXX Completely odd.  Why don't we join blocks around each range?
      joinInclusiveAncestorBlockElements &= deleteResult.unwrap();
    }

    // Check endpoints for possible text deletion.  We can assume that if
    // text node is found, we can delete to end or to begining as
    // appropriate, since the case where both sel endpoints in same text
    // node was already handled (we wouldn't be here)
    nsresult rv = DeleteTextAtStartAndEndOfRange(
        aHTMLEditor, MOZ_KnownLive(aRangesToDelete.FirstRangeRef()));
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoBlockElementsJoiner::DeleteTextAtStartAndEndOfRange() failed");
      return Err(rv);
    }

    if (!joinInclusiveAncestorBlockElements) {
      break;
    }

    AutoInclusiveAncestorBlockElementsJoiner joiner(*mLeftContent,
                                                    *mRightContent);
    Result<bool, nsresult> canJoinThem =
        joiner.Prepare(aHTMLEditor, aEditingHost);
    if (canJoinThem.isErr()) {
      NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Prepare() failed");
      return canJoinThem.propagateErr();
    }

    // If we're joining blocks: if deleting forward the selection should
    // be collapsed to the end of the selection, if deleting backward the
    // selection should be collapsed to the beginning of the selection.
    // But if we're not joining then the selection should collapse to the
    // beginning of the selection if we'redeleting forward, because the
    // end of the selection will still be in the next block. And same
    // thing for deleting backwards (selection should collapse to the end,
    // because the beginning will still be in the first block). See Bug
    // 507936.
    if (aDirectionAndAmount == nsIEditor::eNext) {
      aDirectionAndAmount = nsIEditor::ePrevious;
    } else {
      aDirectionAndAmount = nsIEditor::eNext;
    }

    if (!canJoinThem.inspect()) {
      result.MarkAsCanceled();
      break;
    }

    if (!joiner.CanJoinBlocks()) {
      break;
    }

    Result<EditActionResult, nsresult> joinResult =
        joiner.Run(aHTMLEditor, aEditingHost);
    if (MOZ_UNLIKELY(joinResult.isErr())) {
      NS_WARNING("AutoInclusiveAncestorBlockElementsJoiner::Run() failed");
      return joinResult;
    }
    result |= joinResult.unwrap();
#ifdef DEBUG
    if (joiner.ShouldDeleteLeafContentInstead()) {
      NS_ASSERTION(result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "returning ignored, but returned not ignored");
    } else {
      NS_ASSERTION(!result.Ignored(),
                   "Assumed `AutoInclusiveAncestorBlockElementsJoiner::Run()` "
                   "returning handled, but returned ignored");
    }
#endif  // #ifdef DEBUG
    break;
  }

  nsresult rv =
      mDeleteRangesHandler->DeleteUnnecessaryNodesAndCollapseSelection(
          aHTMLEditor, aDirectionAndAmount,
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->StartRef()),
          EditorDOMPoint(aRangesToDelete.FirstRangeRef()->EndRef()));
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "AutoDeleteRangesHandler::DeleteUnnecessaryNodesAndCollapseSelection() "
        "failed");
    return Err(rv);
  }

  result.MarkAsHandled();
  return result;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::DeleteUnnecessaryNodesAndCollapseSelection(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    const EditorDOMPoint& aSelectionStartPoint,
    const EditorDOMPoint& aSelectionEndPoint) {
  MOZ_ASSERT(aHTMLEditor.IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(EditorUtils::IsEditableContent(
      *aSelectionStartPoint.ContainerAs<nsIContent>(), EditorType::HTML));
  MOZ_ASSERT(EditorUtils::IsEditableContent(
      *aSelectionEndPoint.ContainerAs<nsIContent>(), EditorType::HTML));

  EditorDOMPoint atCaret(aSelectionStartPoint);
  EditorDOMPoint selectionEndPoint(aSelectionEndPoint);

  // If we're handling D&D, this is called to delete dragging item from the
  // tree.  In this case, we should remove parent blocks if it becomes empty.
  if (aHTMLEditor.GetEditAction() == EditAction::eDrop ||
      aHTMLEditor.GetEditAction() == EditAction::eDeleteByDrag) {
    MOZ_ASSERT((atCaret.GetContainer() == selectionEndPoint.GetContainer() &&
                atCaret.Offset() == selectionEndPoint.Offset()) ||
               (atCaret.GetContainer()->GetNextSibling() ==
                    selectionEndPoint.GetContainer() &&
                atCaret.IsEndOfContainer() &&
                selectionEndPoint.IsStartOfContainer()));
    {
      AutoTrackDOMPoint startTracker(aHTMLEditor.RangeUpdaterRef(), &atCaret);
      AutoTrackDOMPoint endTracker(aHTMLEditor.RangeUpdaterRef(),
                                   &selectionEndPoint);

      nsresult rv =
          DeleteParentBlocksWithTransactionIfEmpty(aHTMLEditor, atCaret);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::DeleteParentBlocksWithTransactionIfEmpty() failed");
        return rv;
      }
      aHTMLEditor.TopLevelEditSubActionDataRef().mDidDeleteEmptyParentBlocks =
          rv == NS_OK;
    }
    // If we removed parent blocks, Selection should be collapsed at where
    // the most ancestor empty block has been.
    if (aHTMLEditor.TopLevelEditSubActionDataRef()
            .mDidDeleteEmptyParentBlocks) {
      nsresult rv = aHTMLEditor.CollapseSelectionTo(atCaret);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "EditorBase::CollapseSelectionTo() failed");
      return rv;
    }
  }

  if (NS_WARN_IF(!atCaret.IsInContentNode()) ||
      NS_WARN_IF(!selectionEndPoint.IsInContentNode()) ||
      NS_WARN_IF(!EditorUtils::IsEditableContent(
          *atCaret.ContainerAs<nsIContent>(), EditorType::HTML)) ||
      NS_WARN_IF(!EditorUtils::IsEditableContent(
          *selectionEndPoint.ContainerAs<nsIContent>(), EditorType::HTML))) {
    return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
  }

  // We might have left only collapsed white-space in the start/end nodes
  {
    AutoTrackDOMPoint startTracker(aHTMLEditor.RangeUpdaterRef(), &atCaret);
    AutoTrackDOMPoint endTracker(aHTMLEditor.RangeUpdaterRef(),
                                 &selectionEndPoint);

    nsresult rv = DeleteNodeIfInvisibleAndEditableTextNode(
        aHTMLEditor, MOZ_KnownLive(*atCaret.ContainerAs<nsIContent>()));
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode() "
        "failed to remove start node, but ignored");
    // If we've not handled the selection end container, and it's still
    // editable, let's handle it.
    if (atCaret.ContainerAs<nsIContent>() !=
            selectionEndPoint.ContainerAs<nsIContent>() &&
        EditorUtils::IsEditableContent(
            *selectionEndPoint.ContainerAs<nsIContent>(), EditorType::HTML)) {
      nsresult rv = DeleteNodeIfInvisibleAndEditableTextNode(
          aHTMLEditor,
          MOZ_KnownLive(*selectionEndPoint.ContainerAs<nsIContent>()));
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode() "
          "failed to remove end node, but ignored");
    }
  }

  nsresult rv = aHTMLEditor.CollapseSelectionTo(
      aDirectionAndAmount == nsIEditor::ePrevious ? selectionEndPoint
                                                  : atCaret);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed");
  return rv;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::DeleteNodeIfInvisibleAndEditableTextNode(
    HTMLEditor& aHTMLEditor, nsIContent& aContent) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  Text* text = aContent.GetAsText();
  if (!text) {
    return NS_OK;
  }

  if (!HTMLEditUtils::IsRemovableFromParentNode(*text) ||
      HTMLEditUtils::IsVisibleTextNode(*text)) {
    return NS_OK;
  }

  nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(aContent);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::DeleteNodeWithTransaction() failed");
  return rv;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::DeleteParentBlocksWithTransactionIfEmpty(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aPoint) {
  MOZ_ASSERT(aPoint.IsSet());
  MOZ_ASSERT(aHTMLEditor.mPlaceholderBatch);

  // First, check there is visible contents before the point in current block.
  RefPtr<Element> editingHost = aHTMLEditor.ComputeEditingHost();
  WSRunScanner wsScannerForPoint(editingHost, aPoint);
  if (!wsScannerForPoint.StartsFromCurrentBlockBoundary()) {
    // If there is visible node before the point, we shouldn't remove the
    // parent block.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }
  if (NS_WARN_IF(!wsScannerForPoint.GetStartReasonContent()) ||
      NS_WARN_IF(!wsScannerForPoint.GetStartReasonContent()->GetParentNode())) {
    return NS_ERROR_FAILURE;
  }
  if (editingHost == wsScannerForPoint.GetStartReasonContent()) {
    // If we reach editing host, there is no parent blocks which can be removed.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }
  if (HTMLEditUtils::IsTableCellOrCaption(
          *wsScannerForPoint.GetStartReasonContent())) {
    // If we reach a <td>, <th> or <caption>, we shouldn't remove it even
    // becomes empty because removing such element changes the structure of
    // the <table>.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }

  // Next, check there is visible contents after the point in current block.
  WSScanResult forwardScanFromPointResult =
      wsScannerForPoint.ScanNextVisibleNodeOrBlockBoundaryFrom(aPoint);
  if (forwardScanFromPointResult.Failed()) {
    NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom() failed");
    return NS_ERROR_FAILURE;
  }
  if (forwardScanFromPointResult.ReachedBRElement()) {
    // XXX In my understanding, this is odd.  The end reason may not be
    //     same as the reached <br> element because the equality is
    //     guaranteed only when ReachedCurrentBlockBoundary() returns true.
    //     However, looks like that this code assumes that
    //     GetEndReasonContent() returns the (or a) <br> element.
    NS_ASSERTION(wsScannerForPoint.GetEndReasonContent() ==
                     forwardScanFromPointResult.BRElementPtr(),
                 "End reason is not the reached <br> element");
    // If the <br> element is visible, we shouldn't remove the parent block.
    if (HTMLEditUtils::IsVisibleBRElement(
            *wsScannerForPoint.GetEndReasonContent())) {
      return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
    }
    if (wsScannerForPoint.GetEndReasonContent()->GetNextSibling()) {
      WSScanResult scanResult =
          WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(
              editingHost, EditorRawDOMPoint::After(
                               *wsScannerForPoint.GetEndReasonContent()));
      if (scanResult.Failed()) {
        NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundary() failed");
        return NS_ERROR_FAILURE;
      }
      if (!scanResult.ReachedCurrentBlockBoundary()) {
        // If we couldn't reach the block's end after the invisible <br>,
        // that means that there is visible content.
        return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
      }
    }
  } else if (!forwardScanFromPointResult.ReachedCurrentBlockBoundary()) {
    // If we couldn't reach the block's end, the block has visible content.
    return NS_SUCCESS_EDITOR_ELEMENT_NOT_FOUND;
  }

  // Delete the parent block.
  EditorDOMPoint nextPoint(
      wsScannerForPoint.GetStartReasonContent()->GetParentNode(), 0);
  nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(
      MOZ_KnownLive(*wsScannerForPoint.GetStartReasonContent()));
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
    return rv;
  }
  // If we reach editing host, return NS_OK.
  if (nextPoint.GetContainer() == editingHost) {
    return NS_OK;
  }

  // Otherwise, we need to check whether we're still in empty block or not.

  // If we have mutation event listeners, the next point is now outside of
  // editing host or editing hos has been changed.
  if (aHTMLEditor.MayHaveMutationEventListeners(
          NS_EVENT_BITS_MUTATION_NODEREMOVED |
          NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
          NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED)) {
    Element* newEditingHost = aHTMLEditor.ComputeEditingHost();
    if (NS_WARN_IF(!newEditingHost) ||
        NS_WARN_IF(newEditingHost != editingHost)) {
      return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
    }
    if (NS_WARN_IF(!EditorUtils::IsDescendantOf(*nextPoint.GetContainer(),
                                                *newEditingHost))) {
      return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
    }
  }

  rv = DeleteParentBlocksWithTransactionIfEmpty(aHTMLEditor, nextPoint);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoDeleteRangesHandler::"
                       "DeleteParentBlocksWithTransactionIfEmpty() failed");
  return rv;
}

nsresult
HTMLEditor::AutoDeleteRangesHandler::ComputeRangesToDeleteRangesWithTransaction(
    const HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount,
    AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());

  EditorBase::HowToHandleCollapsedRange howToHandleCollapsedRange =
      EditorBase::HowToHandleCollapsedRangeFor(aDirectionAndAmount);
  if (NS_WARN_IF(aRangesToDelete.IsCollapsed() &&
                 howToHandleCollapsedRange ==
                     EditorBase::HowToHandleCollapsedRange::Ignore)) {
    return NS_ERROR_FAILURE;
  }

  auto extendRangeToSelectCharacterForward =
      [](nsRange& aRange, const EditorRawDOMPointInText& aCaretPoint) -> void {
    const nsTextFragment& textFragment =
        aCaretPoint.ContainerAs<Text>()->TextFragment();
    if (!textFragment.GetLength()) {
      return;
    }
    if (textFragment.IsHighSurrogateFollowedByLowSurrogateAt(
            aCaretPoint.Offset())) {
      DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
          aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset(),
          aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset() + 2);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "nsRange::SetStartAndEnd() failed");
      return;
    }
    DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
        aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset(),
        aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset() + 1);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                         "nsRange::SetStartAndEnd() failed");
  };
  auto extendRangeToSelectCharacterBackward =
      [](nsRange& aRange, const EditorRawDOMPointInText& aCaretPoint) -> void {
    if (aCaretPoint.IsStartOfContainer()) {
      return;
    }
    const nsTextFragment& textFragment =
        aCaretPoint.ContainerAs<Text>()->TextFragment();
    if (!textFragment.GetLength()) {
      return;
    }
    if (textFragment.IsLowSurrogateFollowingHighSurrogateAt(
            aCaretPoint.Offset() - 1)) {
      DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
          aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset() - 2,
          aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset());
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "nsRange::SetStartAndEnd() failed");
      return;
    }
    DebugOnly<nsresult> rvIgnored = aRange.SetStartAndEnd(
        aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset() - 1,
        aCaretPoint.ContainerAs<Text>(), aCaretPoint.Offset());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                         "nsRange::SetStartAndEnd() failed");
  };

  RefPtr<Element> editingHost = aHTMLEditor.ComputeEditingHost();
  for (OwningNonNull<nsRange>& range : aRangesToDelete.Ranges()) {
    // If it's not collapsed, `DeleteRangeTransaction::Create()` will be called
    // with it and `DeleteRangeTransaction` won't modify the range.
    if (!range->Collapsed()) {
      continue;
    }

    if (howToHandleCollapsedRange ==
        EditorBase::HowToHandleCollapsedRange::Ignore) {
      continue;
    }

    // In the other cases, `EditorBase::CreateTransactionForCollapsedRange()`
    // will handle the collapsed range.
    EditorRawDOMPoint caretPoint(range->StartRef());
    if (howToHandleCollapsedRange ==
            EditorBase::HowToHandleCollapsedRange::ExtendBackward &&
        caretPoint.IsStartOfContainer()) {
      nsIContent* previousEditableContent = HTMLEditUtils::GetPreviousContent(
          *caretPoint.GetContainer(), {WalkTreeOption::IgnoreNonEditableNode},
          editingHost);
      if (!previousEditableContent) {
        continue;
      }
      if (!previousEditableContent->IsText()) {
        IgnoredErrorResult ignoredError;
        range->SelectNode(*previousEditableContent, ignoredError);
        NS_WARNING_ASSERTION(!ignoredError.Failed(),
                             "nsRange::SelectNode() failed");
        continue;
      }

      extendRangeToSelectCharacterBackward(
          range,
          EditorRawDOMPointInText::AtEndOf(*previousEditableContent->AsText()));
      continue;
    }

    if (howToHandleCollapsedRange ==
            EditorBase::HowToHandleCollapsedRange::ExtendForward &&
        caretPoint.IsEndOfContainer()) {
      nsIContent* nextEditableContent = HTMLEditUtils::GetNextContent(
          *caretPoint.GetContainer(), {WalkTreeOption::IgnoreNonEditableNode},
          editingHost);
      if (!nextEditableContent) {
        continue;
      }

      if (!nextEditableContent->IsText()) {
        IgnoredErrorResult ignoredError;
        range->SelectNode(*nextEditableContent, ignoredError);
        NS_WARNING_ASSERTION(!ignoredError.Failed(),
                             "nsRange::SelectNode() failed");
        continue;
      }

      extendRangeToSelectCharacterForward(
          range, EditorRawDOMPointInText(nextEditableContent->AsText(), 0));
      continue;
    }

    if (caretPoint.IsInTextNode()) {
      if (howToHandleCollapsedRange ==
          EditorBase::HowToHandleCollapsedRange::ExtendBackward) {
        extendRangeToSelectCharacterBackward(
            range, EditorRawDOMPointInText(caretPoint.ContainerAs<Text>(),
                                           caretPoint.Offset()));
        continue;
      }
      extendRangeToSelectCharacterForward(
          range, EditorRawDOMPointInText(caretPoint.ContainerAs<Text>(),
                                         caretPoint.Offset()));
      continue;
    }

    nsIContent* editableContent =
        howToHandleCollapsedRange ==
                EditorBase::HowToHandleCollapsedRange::ExtendBackward
            ? HTMLEditUtils::GetPreviousContent(
                  caretPoint, {WalkTreeOption::IgnoreNonEditableNode},
                  editingHost)
            : HTMLEditUtils::GetNextContent(
                  caretPoint, {WalkTreeOption::IgnoreNonEditableNode},
                  editingHost);
    if (!editableContent) {
      continue;
    }
    while (editableContent && editableContent->IsCharacterData() &&
           !editableContent->Length()) {
      editableContent =
          howToHandleCollapsedRange ==
                  EditorBase::HowToHandleCollapsedRange::ExtendBackward
              ? HTMLEditUtils::GetPreviousContent(
                    *editableContent, {WalkTreeOption::IgnoreNonEditableNode},
                    editingHost)
              : HTMLEditUtils::GetNextContent(
                    *editableContent, {WalkTreeOption::IgnoreNonEditableNode},
                    editingHost);
    }
    if (!editableContent) {
      continue;
    }

    if (!editableContent->IsText()) {
      IgnoredErrorResult ignoredError;
      range->SelectNode(*editableContent, ignoredError);
      NS_WARNING_ASSERTION(!ignoredError.Failed(),
                           "nsRange::SelectNode() failed");
      continue;
    }

    if (howToHandleCollapsedRange ==
        EditorBase::HowToHandleCollapsedRange::ExtendBackward) {
      extendRangeToSelectCharacterBackward(
          range, EditorRawDOMPointInText::AtEndOf(*editableContent->AsText()));
      continue;
    }
    extendRangeToSelectCharacterForward(
        range, EditorRawDOMPointInText(editableContent->AsText(), 0));
  }

  return NS_OK;
}

template <typename EditorDOMPointType>
nsresult HTMLEditor::DeleteTextAndTextNodesWithTransaction(
    const EditorDOMPointType& aStartPoint, const EditorDOMPointType& aEndPoint,
    TreatEmptyTextNodes aTreatEmptyTextNodes) {
  if (NS_WARN_IF(!aStartPoint.IsSet()) || NS_WARN_IF(!aEndPoint.IsSet())) {
    return NS_ERROR_INVALID_ARG;
  }

  // MOOSE: this routine needs to be modified to preserve the integrity of the
  // wsFragment info.

  if (aStartPoint == aEndPoint) {
    // Nothing to delete
    return NS_OK;
  }

  RefPtr<Element> editingHost = ComputeEditingHost();
  auto deleteEmptyContentNodeWithTransaction =
      [this, &aTreatEmptyTextNodes, &editingHost](nsIContent& aContent)
          MOZ_CAN_RUN_SCRIPT_FOR_DEFINITION -> nsresult {
    OwningNonNull<nsIContent> nodeToRemove = aContent;
    if (aTreatEmptyTextNodes ==
        TreatEmptyTextNodes::RemoveAllEmptyInlineAncestors) {
      Element* emptyParentElementToRemove =
          HTMLEditUtils::GetMostDistantAncestorEditableEmptyInlineElement(
              nodeToRemove, editingHost);
      if (emptyParentElementToRemove) {
        nodeToRemove = *emptyParentElementToRemove;
      }
    }
    nsresult rv = DeleteNodeWithTransaction(nodeToRemove);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "EditorBase::DeleteNodeWithTransaction() failed");
    return rv;
  };

  if (aStartPoint.GetContainer() == aEndPoint.GetContainer() &&
      aStartPoint.IsInTextNode()) {
    if (aTreatEmptyTextNodes !=
            TreatEmptyTextNodes::KeepIfContainerOfRangeBoundaries &&
        aStartPoint.IsStartOfContainer() && aEndPoint.IsEndOfContainer()) {
      nsresult rv = deleteEmptyContentNodeWithTransaction(
          MOZ_KnownLive(*aStartPoint.template ContainerAs<Text>()));
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "deleteEmptyContentNodeWithTransaction() failed");
      return rv;
    }
    RefPtr<Text> textNode = aStartPoint.template ContainerAs<Text>();
    nsresult rv =
        DeleteTextWithTransaction(*textNode, aStartPoint.Offset(),
                                  aEndPoint.Offset() - aStartPoint.Offset());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::DeleteTextWithTransaction() failed");
    return rv;
  }

  RefPtr<nsRange> range =
      nsRange::Create(aStartPoint.ToRawRangeBoundary(),
                      aEndPoint.ToRawRangeBoundary(), IgnoreErrors());
  if (!range) {
    NS_WARNING("nsRange::Create() failed");
    return NS_ERROR_FAILURE;
  }

  // Collect editable text nodes in the given range.
  AutoTArray<OwningNonNull<Text>, 16> arrayOfTextNodes;
  DOMIterator iter;
  if (NS_FAILED(iter.Init(*range))) {
    return NS_OK;  // Nothing to delete in the range.
  }
  iter.AppendNodesToArray(
      +[](nsINode& aNode, void*) {
        MOZ_ASSERT(aNode.IsText());
        return HTMLEditUtils::IsSimplyEditableNode(aNode);
      },
      arrayOfTextNodes);
  for (OwningNonNull<Text>& textNode : arrayOfTextNodes) {
    if (textNode == aStartPoint.GetContainer()) {
      if (aStartPoint.IsEndOfContainer()) {
        continue;
      }
      if (aStartPoint.IsStartOfContainer() &&
          aTreatEmptyTextNodes !=
              TreatEmptyTextNodes::KeepIfContainerOfRangeBoundaries) {
        nsresult rv = deleteEmptyContentNodeWithTransaction(
            MOZ_KnownLive(*aStartPoint.template ContainerAs<Text>()));
        if (NS_FAILED(rv)) {
          NS_WARNING("deleteEmptyContentNodeWithTransaction() failed");
          return rv;
        }
        continue;
      }
      nsresult rv = DeleteTextWithTransaction(
          MOZ_KnownLive(textNode), aStartPoint.Offset(),
          textNode->Length() - aStartPoint.Offset());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
        return rv;
      }
      continue;
    }

    if (textNode == aEndPoint.GetContainer()) {
      if (aEndPoint.IsStartOfContainer()) {
        break;
      }
      if (aEndPoint.IsEndOfContainer() &&
          aTreatEmptyTextNodes !=
              TreatEmptyTextNodes::KeepIfContainerOfRangeBoundaries) {
        nsresult rv = deleteEmptyContentNodeWithTransaction(
            MOZ_KnownLive(*aEndPoint.template ContainerAs<Text>()));
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "deleteEmptyContentNodeWithTransaction() failed");
        return rv;
      }
      nsresult rv = DeleteTextWithTransaction(MOZ_KnownLive(textNode), 0,
                                              aEndPoint.Offset());
      if (NS_WARN_IF(Destroyed())) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "HTMLEditor::DeleteTextWithTransaction() failed");
      return rv;
    }

    nsresult rv =
        deleteEmptyContentNodeWithTransaction(MOZ_KnownLive(textNode));
    if (NS_FAILED(rv)) {
      NS_WARNING("deleteEmptyContentNodeWithTransaction() failed");
      return rv;
    }
  }

  return NS_OK;
}

Result<EditorDOMPoint, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::JoinNodesDeepWithTransaction(
        HTMLEditor& aHTMLEditor, nsIContent& aLeftContent,
        nsIContent& aRightContent) {
  // While the rightmost children and their descendants of the left node match
  // the leftmost children and their descendants of the right node, join them
  // up.

  nsCOMPtr<nsIContent> leftContentToJoin = &aLeftContent;
  nsCOMPtr<nsIContent> rightContentToJoin = &aRightContent;
  nsCOMPtr<nsINode> parentNode = aRightContent.GetParentNode();

  EditorDOMPoint ret;
  const HTMLEditUtils::StyleDifference kCompareStyle =
      aHTMLEditor.IsCSSEnabled() ? StyleDifference::CompareIfSpanElements
                                 : StyleDifference::Ignore;
  while (leftContentToJoin && rightContentToJoin && parentNode &&
         HTMLEditUtils::CanContentsBeJoined(
             *leftContentToJoin, *rightContentToJoin, kCompareStyle)) {
    // Do the join
    Result<JoinNodesResult, nsresult> joinNodesResult =
        aHTMLEditor.JoinNodesWithTransaction(*leftContentToJoin,
                                             *rightContentToJoin);
    if (MOZ_UNLIKELY(joinNodesResult.isErr())) {
      NS_WARNING("HTMLEditor::JoinNodesWithTransaction() failed");
      return joinNodesResult.propagateErr();
    }

    ret = joinNodesResult.inspect().AtJoinedPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!ret.IsSet())) {
      return Err(NS_ERROR_FAILURE);
    }

    if (parentNode->IsText()) {
      // We've joined all the way down to text nodes, we're done!
      return ret;
    }

    // Get new left and right nodes, and begin anew
    rightContentToJoin = ret.GetCurrentChildAtOffset();
    if (rightContentToJoin) {
      leftContentToJoin = rightContentToJoin->GetPreviousSibling();
    } else {
      leftContentToJoin = nullptr;
    }

    // Skip over non-editable nodes
    while (leftContentToJoin && !EditorUtils::IsEditableContent(
                                    *leftContentToJoin, EditorType::HTML)) {
      leftContentToJoin = leftContentToJoin->GetPreviousSibling();
    }
    if (!leftContentToJoin) {
      return ret;
    }

    while (rightContentToJoin && !EditorUtils::IsEditableContent(
                                     *rightContentToJoin, EditorType::HTML)) {
      rightContentToJoin = rightContentToJoin->GetNextSibling();
    }
    if (!rightContentToJoin) {
      return ret;
    }
  }

  if (!ret.IsSet()) {
    NS_WARNING("HTMLEditor::JoinNodesDeepWithTransaction() joined no contents");
    return Err(NS_ERROR_FAILURE);
  }
  return ret;
}

Result<bool, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::AutoInclusiveAncestorBlockElementsJoiner::Prepare(
        const HTMLEditor& aHTMLEditor, const Element& aEditingHost) {
  mLeftBlockElement = HTMLEditUtils::GetInclusiveAncestorElement(
      mInclusiveDescendantOfLeftBlockElement,
      HTMLEditUtils::ClosestEditableBlockElementExceptHRElement);
  mRightBlockElement = HTMLEditUtils::GetInclusiveAncestorElement(
      mInclusiveDescendantOfRightBlockElement,
      HTMLEditUtils::ClosestEditableBlockElementExceptHRElement);

  if (NS_WARN_IF(!IsSet())) {
    mCanJoinBlocks = false;
    return Err(NS_ERROR_UNEXPECTED);
  }

  // Don't join the blocks if both of them are basic structure of the HTML
  // document (Note that `<body>` can be joined with its children).
  if (mLeftBlockElement->IsAnyOfHTMLElements(nsGkAtoms::html, nsGkAtoms::head,
                                             nsGkAtoms::body) &&
      mRightBlockElement->IsAnyOfHTMLElements(nsGkAtoms::html, nsGkAtoms::head,
                                              nsGkAtoms::body)) {
    mCanJoinBlocks = false;
    return false;
  }

  if (HTMLEditUtils::IsAnyTableElement(mLeftBlockElement) ||
      HTMLEditUtils::IsAnyTableElement(mRightBlockElement)) {
    // Do not try to merge table elements, cancel the deletion.
    mCanJoinBlocks = false;
    return false;
  }

  // Bail if both blocks the same
  if (IsSameBlockElement()) {
    mCanJoinBlocks = true;  // XXX Anyway, Run() will ingore this case.
    mFallbackToDeleteLeafContent = true;
    return true;
  }

  // Joining a list item to its parent is a NOP.
  if (HTMLEditUtils::IsAnyListElement(mLeftBlockElement) &&
      HTMLEditUtils::IsListItem(mRightBlockElement) &&
      mRightBlockElement->GetParentNode() == mLeftBlockElement) {
    mCanJoinBlocks = false;
    return true;
  }

  // Special rule here: if we are trying to join list items, and they are in
  // different lists, join the lists instead.
  if (HTMLEditUtils::IsListItem(mLeftBlockElement) &&
      HTMLEditUtils::IsListItem(mRightBlockElement)) {
    // XXX leftListElement and/or rightListElement may be not list elements.
    Element* leftListElement = mLeftBlockElement->GetParentElement();
    Element* rightListElement = mRightBlockElement->GetParentElement();
    EditorDOMPoint atChildInBlock;
    if (leftListElement && rightListElement &&
        leftListElement != rightListElement &&
        !EditorUtils::IsDescendantOf(*leftListElement, *mRightBlockElement,
                                     &atChildInBlock) &&
        !EditorUtils::IsDescendantOf(*rightListElement, *mLeftBlockElement,
                                     &atChildInBlock)) {
      // There are some special complications if the lists are descendants of
      // the other lists' items.  Note that it is okay for them to be
      // descendants of the other lists themselves, which is the usual case for
      // sublists in our implementation.
      MOZ_DIAGNOSTIC_ASSERT(!atChildInBlock.IsSet());
      mLeftBlockElement = leftListElement;
      mRightBlockElement = rightListElement;
      mNewListElementTagNameOfRightListElement =
          Some(leftListElement->NodeInfo()->NameAtom());
    }
  }

  if (!EditorUtils::IsDescendantOf(*mLeftBlockElement, *mRightBlockElement,
                                   &mPointContainingTheOtherBlockElement)) {
    Unused << EditorUtils::IsDescendantOf(
        *mRightBlockElement, *mLeftBlockElement,
        &mPointContainingTheOtherBlockElement);
  }

  if (mPointContainingTheOtherBlockElement.GetContainer() ==
      mRightBlockElement) {
    mPrecedingInvisibleBRElement =
        WSRunScanner::GetPrecedingBRElementUnlessVisibleContentFound(
            aHTMLEditor.ComputeEditingHost(),
            EditorDOMPoint::AtEndOf(mLeftBlockElement));
    // `WhiteSpaceVisibilityKeeper::
    // MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement()`
    // returns ignored when:
    // - No preceding invisible `<br>` element and
    // - mNewListElementTagNameOfRightListElement is nothing and
    // - There is no content to move from right block element.
    if (!mPrecedingInvisibleBRElement) {
      if (CanMergeLeftAndRightBlockElements()) {
        // Always marked as handled in this case.
        mFallbackToDeleteLeafContent = false;
      } else {
        // Marked as handled only when it actually moves a content node.
        Result<bool, nsresult> firstLineHasContent =
            aHTMLEditor.CanMoveOrDeleteSomethingInHardLine(
                mPointContainingTheOtherBlockElement
                    .NextPoint<EditorDOMPoint>(),
                aEditingHost);
        mFallbackToDeleteLeafContent =
            firstLineHasContent.isOk() && !firstLineHasContent.inspect();
      }
    } else {
      // Marked as handled when deleting the invisible `<br>` element.
      mFallbackToDeleteLeafContent = false;
    }
  } else if (mPointContainingTheOtherBlockElement.GetContainer() ==
             mLeftBlockElement) {
    mPrecedingInvisibleBRElement =
        WSRunScanner::GetPrecedingBRElementUnlessVisibleContentFound(
            aHTMLEditor.ComputeEditingHost(),
            mPointContainingTheOtherBlockElement);
    // `WhiteSpaceVisibilityKeeper::
    // MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement()`
    // returns ignored when:
    // - No preceding invisible `<br>` element and
    // - mNewListElementTagNameOfRightListElement is some and
    // - The right block element has no children
    // or,
    // - No preceding invisible `<br>` element and
    // - mNewListElementTagNameOfRightListElement is nothing and
    // - There is no content to move from right block element.
    if (!mPrecedingInvisibleBRElement) {
      if (CanMergeLeftAndRightBlockElements()) {
        // Marked as handled only when it actualy moves a content node.
        Result<bool, nsresult> rightBlockHasContent =
            aHTMLEditor.CanMoveChildren(*mRightBlockElement,
                                        *mLeftBlockElement);
        mFallbackToDeleteLeafContent =
            rightBlockHasContent.isOk() && !rightBlockHasContent.inspect();
      } else {
        // Marked as handled only when it actually moves a content node.
        Result<bool, nsresult> firstLineHasContent =
            aHTMLEditor.CanMoveOrDeleteSomethingInHardLine(
                EditorDOMPoint(mRightBlockElement, 0u), aEditingHost);
        mFallbackToDeleteLeafContent =
            firstLineHasContent.isOk() && !firstLineHasContent.inspect();
      }
    } else {
      // Marked as handled when deleting the invisible `<br>` element.
      mFallbackToDeleteLeafContent = false;
    }
  } else {
    mPrecedingInvisibleBRElement =
        WSRunScanner::GetPrecedingBRElementUnlessVisibleContentFound(
            aHTMLEditor.ComputeEditingHost(),
            EditorDOMPoint::AtEndOf(mLeftBlockElement));
    // `WhiteSpaceVisibilityKeeper::
    // MergeFirstLineOfRightBlockElementIntoLeftBlockElement()` always
    // return "handled".
    mFallbackToDeleteLeafContent = false;
  }

  mCanJoinBlocks = true;
  return true;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    AutoInclusiveAncestorBlockElementsJoiner::ComputeRangesToDelete(
        const HTMLEditor& aHTMLEditor, const EditorDOMPoint& aCaretPoint,
        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(!aRangesToDelete.Ranges().IsEmpty());
  MOZ_ASSERT(mLeftBlockElement);
  MOZ_ASSERT(mRightBlockElement);

  if (IsSameBlockElement()) {
    if (!aCaretPoint.IsSet()) {
      return NS_OK;  // The ranges are not collapsed, keep them as-is.
    }
    nsresult rv = aRangesToDelete.Collapse(aCaretPoint);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  EditorDOMPoint pointContainingTheOtherBlock;
  if (!EditorUtils::IsDescendantOf(*mLeftBlockElement, *mRightBlockElement,
                                   &pointContainingTheOtherBlock)) {
    Unused << EditorUtils::IsDescendantOf(
        *mRightBlockElement, *mLeftBlockElement, &pointContainingTheOtherBlock);
  }
  EditorDOMRange range =
      WSRunScanner::GetRangeForDeletingBlockElementBoundaries(
          aHTMLEditor, *mLeftBlockElement, *mRightBlockElement,
          pointContainingTheOtherBlock);
  if (!range.IsPositioned()) {
    NS_WARNING(
        "WSRunScanner::GetRangeForDeletingBlockElementBoundaries() failed");
    return NS_ERROR_FAILURE;
  }
  if (!aCaretPoint.IsSet()) {
    // Don't shrink the original range.
    bool noNeedToChangeStart = false;
    const auto atStart =
        aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (atStart.IsBefore(range.StartRef())) {
      // If the range starts from end of a container, and computed block
      // boundaries range starts from an invisible `<br>` element,  we
      // may need to shrink the range.
      Element* editingHost = aHTMLEditor.ComputeEditingHost();
      NS_WARNING_ASSERTION(editingHost, "There was no editing host");
      nsIContent* nextContent =
          atStart.IsEndOfContainer() && range.StartRef().GetChild() &&
                  HTMLEditUtils::IsInvisibleBRElement(
                      *range.StartRef().GetChild())
              ? HTMLEditUtils::GetNextContent(
                    *atStart.ContainerAs<nsIContent>(),
                    {WalkTreeOption::IgnoreDataNodeExceptText,
                     WalkTreeOption::StopAtBlockBoundary},
                    editingHost)
              : nullptr;
      if (!nextContent || nextContent != range.StartRef().GetChild()) {
        noNeedToChangeStart = true;
        range.SetStart(
            aRangesToDelete.GetFirstRangeStartPoint<EditorDOMPoint>());
      }
    }
    if (range.EndRef().IsBefore(
            aRangesToDelete.GetFirstRangeEndPoint<EditorRawDOMPoint>())) {
      if (noNeedToChangeStart) {
        return NS_OK;  // We don't need to modify the range.
      }
      range.SetEnd(aRangesToDelete.GetFirstRangeEndPoint<EditorDOMPoint>());
    }
  }
  // XXX Oddly, we join blocks only at the first range.
  nsresult rv = aRangesToDelete.FirstRangeRef()->SetStartAndEnd(
      range.StartRef().ToRawRangeBoundary(),
      range.EndRef().ToRawRangeBoundary());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "AutoRangeArray::SetStartAndEnd() failed");
  return rv;
}

Result<EditActionResult, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoBlockElementsJoiner::AutoInclusiveAncestorBlockElementsJoiner::Run(
        HTMLEditor& aHTMLEditor, const Element& aEditingHost) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(mLeftBlockElement);
  MOZ_ASSERT(mRightBlockElement);

  if (IsSameBlockElement()) {
    return EditActionResult::IgnoredResult();
  }

  if (!mCanJoinBlocks) {
    return EditActionResult::HandledResult();
  }

  // If the left block element is in the right block element, move the hard
  // line including the right block element to end of the left block.
  // However, if we are merging list elements, we don't join them.
  if (mPointContainingTheOtherBlockElement.GetContainer() ==
      mRightBlockElement) {
    Result<EditActionResult, nsresult> result = WhiteSpaceVisibilityKeeper::
        MergeFirstLineOfRightBlockElementIntoDescendantLeftBlockElement(
            aHTMLEditor, MOZ_KnownLive(*mLeftBlockElement),
            MOZ_KnownLive(*mRightBlockElement),
            mPointContainingTheOtherBlockElement,
            mNewListElementTagNameOfRightListElement,
            MOZ_KnownLive(mPrecedingInvisibleBRElement), aEditingHost);
    NS_WARNING_ASSERTION(result.isOk(),
                         "WhiteSpaceVisibilityKeeper::"
                         "MergeFirstLineOfRightBlockElementIntoDescendantLeftBl"
                         "ockElement() failed");
    return result;
  }

  // If the right block element is in the left block element:
  // - move list item elements in the right block element to where the left
  //   list element is
  // - or first hard line in the right block element to where:
  //   - the left block element is.
  //   - or the given left content in the left block is.
  if (mPointContainingTheOtherBlockElement.GetContainer() ==
      mLeftBlockElement) {
    Result<EditActionResult, nsresult> result = WhiteSpaceVisibilityKeeper::
        MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement(
            aHTMLEditor, MOZ_KnownLive(*mLeftBlockElement),
            MOZ_KnownLive(*mRightBlockElement),
            mPointContainingTheOtherBlockElement,
            MOZ_KnownLive(*mInclusiveDescendantOfLeftBlockElement),
            mNewListElementTagNameOfRightListElement,
            MOZ_KnownLive(mPrecedingInvisibleBRElement), aEditingHost);
    NS_WARNING_ASSERTION(result.isOk(),
                         "WhiteSpaceVisibilityKeeper::"
                         "MergeFirstLineOfRightBlockElementIntoAncestorLeftBloc"
                         "kElement() failed");
    return result;
  }

  MOZ_ASSERT(!mPointContainingTheOtherBlockElement.IsSet());

  // Normal case.  Blocks are siblings, or at least close enough.  An example
  // of the latter is <p>paragraph</p><ul><li>one<li>two<li>three</ul>.  The
  // first li and the p are not true siblings, but we still want to join them
  // if you backspace from li into p.
  Result<EditActionResult, nsresult> result = WhiteSpaceVisibilityKeeper::
      MergeFirstLineOfRightBlockElementIntoLeftBlockElement(
          aHTMLEditor, MOZ_KnownLive(*mLeftBlockElement),
          MOZ_KnownLive(*mRightBlockElement),
          mNewListElementTagNameOfRightListElement,
          MOZ_KnownLive(mPrecedingInvisibleBRElement), aEditingHost);
  NS_WARNING_ASSERTION(
      result.isOk(),
      "WhiteSpaceVisibilityKeeper::"
      "MergeFirstLineOfRightBlockElementIntoLeftBlockElement() failed");
  return result;
}

Result<bool, nsresult> HTMLEditor::CanMoveOrDeleteSomethingInHardLine(
    const EditorDOMPoint& aPointInHardLine, const Element& aEditingHost) const {
  if (MOZ_UNLIKELY(NS_WARN_IF(!aPointInHardLine.IsSet()) ||
                   NS_WARN_IF(aPointInHardLine.IsInNativeAnonymousSubtree()))) {
    return Err(NS_ERROR_INVALID_ARG);
  }

  RefPtr<nsRange> oneLineRange =
      AutoRangeArray::CreateRangeWrappingStartAndEndLinesContainingBoundaries(
          aPointInHardLine, aPointInHardLine,
          EditSubAction::eMergeBlockContents, aEditingHost);
  if (!oneLineRange || oneLineRange->Collapsed() ||
      !oneLineRange->IsPositioned() ||
      !oneLineRange->GetStartContainer()->IsContent() ||
      !oneLineRange->GetEndContainer()->IsContent()) {
    return false;
  }

  // If there is only a padding `<br>` element in a empty block, it's selected
  // by `UpdatePointsToSelectAllChildrenIfCollapsedInEmptyBlockElement()`.
  // However, it won't be moved.  Although it'll be deleted,
  // `MoveOneHardLineContentsWithTransaction()` returns "ignored".  Therefore,
  // we should return `false` in this case.
  if (nsIContent* childContent = oneLineRange->GetChildAtStartOffset()) {
    if (childContent->IsHTMLElement(nsGkAtoms::br) &&
        childContent->GetParent()) {
      if (const Element* blockElement =
              HTMLEditUtils::GetInclusiveAncestorElement(
                  *childContent->GetParent(),
                  HTMLEditUtils::ClosestBlockElement)) {
        if (HTMLEditUtils::IsEmptyNode(*blockElement)) {
          return false;
        }
      }
    }
  }

  nsINode* commonAncestor = oneLineRange->GetClosestCommonInclusiveAncestor();
  // Currently, we move non-editable content nodes too.
  EditorRawDOMPoint startPoint(oneLineRange->StartRef());
  if (!startPoint.IsEndOfContainer()) {
    return true;
  }
  EditorRawDOMPoint endPoint(oneLineRange->EndRef());
  if (!endPoint.IsStartOfContainer()) {
    return true;
  }
  if (startPoint.GetContainer() != commonAncestor) {
    while (true) {
      EditorRawDOMPoint pointInParent(startPoint.GetContainerAs<nsIContent>());
      if (NS_WARN_IF(!pointInParent.IsInContentNode())) {
        return Err(NS_ERROR_FAILURE);
      }
      if (pointInParent.GetContainer() == commonAncestor) {
        startPoint = pointInParent;
        break;
      }
      if (!pointInParent.IsEndOfContainer()) {
        return true;
      }
    }
  }
  if (endPoint.GetContainer() != commonAncestor) {
    while (true) {
      EditorRawDOMPoint pointInParent(endPoint.GetContainerAs<nsIContent>());
      if (NS_WARN_IF(!pointInParent.IsInContentNode())) {
        return Err(NS_ERROR_FAILURE);
      }
      if (pointInParent.GetContainer() == commonAncestor) {
        endPoint = pointInParent;
        break;
      }
      if (!pointInParent.IsStartOfContainer()) {
        return true;
      }
    }
  }
  // If start point and end point in the common ancestor are direct siblings,
  // there is no content to move or delete.
  // E.g., `<b>abc<br>[</b><i>]<br>def</i>`.
  return startPoint.GetNextSiblingOfChild() != endPoint.GetChild();
}

Result<MoveNodeResult, nsresult>
HTMLEditor::MoveOneHardLineContentsWithTransaction(
    const EditorDOMPoint& aPointInHardLine,
    const EditorDOMPoint& aPointToInsert, const Element& aEditingHost,
    MoveToEndOfContainer
        aMoveToEndOfContainer /* = MoveToEndOfContainer::No */) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointInHardLine.IsInContentNode());
  MOZ_ASSERT(aPointToInsert.IsSetAndValid());

  if (NS_WARN_IF(aPointToInsert.IsInNativeAnonymousSubtree())) {
    return Err(NS_ERROR_INVALID_ARG);
  }

  const RefPtr<Element> srcInclusiveAncestorBlock =
      aPointInHardLine.IsInContentNode()
          ? HTMLEditUtils::GetInclusiveAncestorElement(
                *aPointInHardLine.ContainerAs<nsIContent>(),
                HTMLEditUtils::ClosestBlockElement)
          : nullptr;
  const RefPtr<Element> destInclusiveAncestorBlock =
      aPointToInsert.IsInContentNode()
          ? HTMLEditUtils::GetInclusiveAncestorElement(
                *aPointToInsert.ContainerAs<nsIContent>(),
                HTMLEditUtils::ClosestBlockElement)
          : nullptr;
  const bool movingToParentBlock =
      destInclusiveAncestorBlock && srcInclusiveAncestorBlock &&
      destInclusiveAncestorBlock != srcInclusiveAncestorBlock &&
      srcInclusiveAncestorBlock->IsInclusiveDescendantOf(
          destInclusiveAncestorBlock);
  const RefPtr<Element> topmostSrcAncestorBlockInDestBlock = [&]() -> Element* {
    if (!movingToParentBlock) {
      return nullptr;
    }
    Element* lastBlockAncestor = srcInclusiveAncestorBlock;
    for (Element* element :
         srcInclusiveAncestorBlock->InclusiveAncestorsOfType<Element>()) {
      if (element == destInclusiveAncestorBlock) {
        return lastBlockAncestor;
      }
      if (HTMLEditUtils::IsBlockElement(*lastBlockAncestor)) {
        lastBlockAncestor = element;
      }
    }
    return nullptr;
  }();
  MOZ_ASSERT_IF(movingToParentBlock, topmostSrcAncestorBlockInDestBlock);

  // If we move content from or to <pre>, we don't need to preserve the
  // white-space style for compatibility with both our traditional behavior
  // and the other browsers.
  const PreserveWhiteSpaceStyle preserveWhiteSpaceStyle = [&]() {
    if (MOZ_UNLIKELY(!destInclusiveAncestorBlock)) {
      return PreserveWhiteSpaceStyle::No;
    }
    // TODO: If `white-space` is specified by non-UA stylesheet, we should
    // preserve it even if the right block is <pre> for compatibility with the
    // other browsers.
    const auto IsInclusiveDescendantOfPre = [](const nsIContent& aContent) {
      // If the content has different `white-space` style from <pre>, we
      // shouldn't treat it as a descendant of <pre> because web apps or
      // the user intent to treat the white-spaces in aContent not as `pre`.
      if (EditorUtils::GetComputedWhiteSpaceStyle(aContent).valueOr(
              StyleWhiteSpace::Normal) != StyleWhiteSpace::Pre) {
        return false;
      }
      for (const Element* element :
           aContent.InclusiveAncestorsOfType<Element>()) {
        if (element->IsHTMLElement(nsGkAtoms::pre)) {
          return true;
        }
      }
      return false;
    };
    if (IsInclusiveDescendantOfPre(*destInclusiveAncestorBlock) ||
        MOZ_UNLIKELY(!aPointInHardLine.IsInContentNode()) ||
        IsInclusiveDescendantOfPre(
            *aPointInHardLine.ContainerAs<nsIContent>())) {
      return PreserveWhiteSpaceStyle::No;
    }
    return PreserveWhiteSpaceStyle::Yes;
  }();

  EditorDOMPoint pointToInsert(aPointToInsert);
  EditorDOMPoint pointToPutCaret;
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  {
    AutoTrackDOMPoint tackPointToInsert(RangeUpdaterRef(), &pointToInsert);

    {
      AutoRangeArray rangesToWrapTheLine(aPointInHardLine);
      rangesToWrapTheLine.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
          EditSubAction::eMergeBlockContents, aEditingHost);
      Result<EditorDOMPoint, nsresult> splitResult =
          rangesToWrapTheLine
              .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
                  *this);
      if (MOZ_UNLIKELY(splitResult.isErr())) {
        NS_WARNING(
            "AutoRangeArray::"
            "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries()"
            " failed");
        return Err(splitResult.unwrapErr());
      }
      if (splitResult.inspect().IsSet()) {
        pointToPutCaret = splitResult.unwrap();
      }
      nsresult rv = rangesToWrapTheLine.CollectEditTargetNodes(
          *this, arrayOfContents, EditSubAction::eMergeBlockContents,
          AutoRangeArray::CollectNonEditableNodes::Yes);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "AutoRangeArray::CollectEditTargetNodes(EditSubAction::"
            "eMergeBlockContents, CollectNonEditableNodes::Yes) failed");
        return Err(rv);
      }
    }

    Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
        MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                           EditSubAction::eMergeBlockContents);
    if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
          "eMergeBlockContents) failed");
      return splitAtBRElementsResult.propagateErr();
    }
    if (splitAtBRElementsResult.inspect().IsSet()) {
      pointToPutCaret = splitAtBRElementsResult.unwrap();
    }
  }

  if (!pointToInsert.IsSetAndValid()) {
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  if (AllowsTransactionsToChangeSelection() && pointToPutCaret.IsSet()) {
    nsresult rv = CollapseSelectionTo(pointToPutCaret);
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::CollapseSelectionTo() failed");
      return Err(rv);
    }
  }

  if (arrayOfContents.IsEmpty()) {
    return MoveNodeResult::IgnoredResult(std::move(pointToInsert));
  }

  // Track the range which contains the moved contents.
  EditorDOMRange movedContentRange(pointToInsert);
  MoveNodeResult moveContentsInLineResult =
      MoveNodeResult::IgnoredResult(pointToInsert);
  if (aMoveToEndOfContainer == MoveToEndOfContainer::Yes) {
    pointToInsert.SetToEndOf(pointToInsert.GetContainer());
  }
  for (const OwningNonNull<nsIContent>& content : arrayOfContents) {
    {
      AutoEditorDOMRangeChildrenInvalidator lockOffsets(movedContentRange);
      // If the content is a block element, move all children of it to the
      // new container, and then, remove the (probably) empty block element.
      if (HTMLEditUtils::IsBlockElement(content)) {
        Result<MoveNodeResult, nsresult> moveChildrenResult =
            MoveChildrenWithTransaction(MOZ_KnownLive(*content->AsElement()),
                                        pointToInsert, preserveWhiteSpaceStyle);
        if (MOZ_UNLIKELY(moveChildrenResult.isErr())) {
          NS_WARNING("HTMLEditor::MoveChildrenWithTransaction() failed");
          return moveChildrenResult;
        }
        moveContentsInLineResult |= moveChildrenResult.inspect();
        moveContentsInLineResult.MarkAsHandled();
        // MOZ_KnownLive due to bug 1622253
        nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(content));
        if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
          moveContentsInLineResult.IgnoreCaretPointSuggestion();
          return Err(NS_ERROR_EDITOR_DESTROYED);
        }
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rv),
            "EditorBase::DeleteNodeWithTransaction() failed, but ignored");
      }
      // If the moving content is empty inline node, we don't want it to appear
      // in the dist paragraph.
      else if (HTMLEditUtils::IsEmptyInlineContainer(
                   content, {EmptyCheckOption::TreatSingleBRElementAsVisible,
                             EmptyCheckOption::TreatListItemAsVisible,
                             EmptyCheckOption::TreatTableCellAsVisible})) {
        nsCOMPtr<nsIContent> emptyContent =
            HTMLEditUtils::GetMostDistantAncestorEditableEmptyInlineElement(
                content, &aEditingHost);
        if (!emptyContent) {
          emptyContent = content;
        }
        nsresult rv = DeleteNodeWithTransaction(*emptyContent);
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
          return Err(rv);
        }
      } else {
        // MOZ_KnownLive due to bug 1622253
        Result<MoveNodeResult, nsresult> moveNodeOrChildrenResult =
            MoveNodeOrChildrenWithTransaction(
                MOZ_KnownLive(content), pointToInsert, preserveWhiteSpaceStyle);
        if (MOZ_UNLIKELY(moveNodeOrChildrenResult.isErr())) {
          NS_WARNING("HTMLEditor::MoveNodeOrChildrenWithTransaction() failed");
          return moveNodeOrChildrenResult;
        }
        moveContentsInLineResult |= moveNodeOrChildrenResult.inspect();
      }
    }
    // For backward compatibility, we should move contents to end of the
    // container if this is called with MoveToEndOfContainer::Yes.
    // And also if pointToInsert has been made invalid with removing preceding
    // children, we should move the content to the end of the container.
    if (aMoveToEndOfContainer == MoveToEndOfContainer::Yes ||
        (MayHaveMutationEventListeners() &&
         MOZ_UNLIKELY(!moveContentsInLineResult.NextInsertionPointRef()
                           .IsSetAndValid()))) {
      pointToInsert.SetToEndOf(pointToInsert.GetContainer());
    } else {
      MOZ_DIAGNOSTIC_ASSERT(
          moveContentsInLineResult.NextInsertionPointRef().IsSet());
      pointToInsert = moveContentsInLineResult.NextInsertionPointRef();
    }
    if (!MayHaveMutationEventListeners() ||
        movedContentRange.EndRef().IsBefore(pointToInsert)) {
      movedContentRange.SetEnd(pointToInsert);
    }
  }

  // Nothing has been moved, we don't need to clean up unnecessary <br> element.
  // And also if we're not moving content into a block, we can quit right now.
  if (moveContentsInLineResult.Ignored() ||
      MOZ_UNLIKELY(!destInclusiveAncestorBlock)) {
    return moveContentsInLineResult;
  }

  // If we couldn't track the range to clean up, we should just stop cleaning up
  // because returning error from here may change the behavior of web apps using
  // mutation event listeners.
  if (MOZ_UNLIKELY(!movedContentRange.IsPositioned() ||
                   movedContentRange.Collapsed())) {
    return moveContentsInLineResult;
  }

  // If we didn't preserve white-space for backward compatibility and
  // white-space becomes not preformatted, we need to clean it up the last text
  // node if it ends with a preformatted line break.
  if (preserveWhiteSpaceStyle == PreserveWhiteSpaceStyle::No) {
    const RefPtr<Text> textNodeEndingWithUnnecessaryLineBreak = [&]() -> Text* {
      Text* lastTextNode = Text::FromNodeOrNull(
          movingToParentBlock ? HTMLEditUtils::GetPreviousContent(
                                    *topmostSrcAncestorBlockInDestBlock,
                                    {WalkTreeOption::StopAtBlockBoundary},
                                    destInclusiveAncestorBlock)
                              : HTMLEditUtils::GetLastLeafContent(
                                    *destInclusiveAncestorBlock,
                                    {LeafNodeType::LeafNodeOrNonEditableNode}));
      if (!lastTextNode ||
          !HTMLEditUtils::IsSimplyEditableNode(*lastTextNode)) {
        return nullptr;
      }
      const nsTextFragment& textFragment = lastTextNode->TextFragment();
      const char16_t lastCh =
          textFragment.GetLength()
              ? textFragment.CharAt(textFragment.GetLength() - 1u)
              : 0;
      return lastCh == HTMLEditUtils::kNewLine &&
                     !EditorUtils::IsNewLinePreformatted(*lastTextNode)
                 ? lastTextNode
                 : nullptr;
    }();
    if (textNodeEndingWithUnnecessaryLineBreak) {
      if (textNodeEndingWithUnnecessaryLineBreak->TextDataLength() == 1u) {
        const RefPtr<Element> inlineElement =
            HTMLEditUtils::GetMostDistantAncestorEditableEmptyInlineElement(
                *textNodeEndingWithUnnecessaryLineBreak, &aEditingHost);
        nsresult rv = DeleteNodeWithTransaction(
            inlineElement ? static_cast<nsIContent&>(*inlineElement)
                          : static_cast<nsIContent&>(
                                *textNodeEndingWithUnnecessaryLineBreak));
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
          return Err(rv);
        }
      } else {
        nsresult rv = DeleteTextWithTransaction(
            *textNodeEndingWithUnnecessaryLineBreak,
            textNodeEndingWithUnnecessaryLineBreak->TextDataLength() - 1u, 1u);
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
          return Err(rv);
        }
      }
    }
  }

  nsCOMPtr<nsIContent> lastLineBreakContent =
      movingToParentBlock
          ? HTMLEditUtils::GetUnnecessaryLineBreakContent(
                *topmostSrcAncestorBlockInDestBlock, ScanLineBreak::BeforeBlock)
          : HTMLEditUtils::GetUnnecessaryLineBreakContent(
                *destInclusiveAncestorBlock, ScanLineBreak::AtEndOfBlock);
  if (!lastLineBreakContent) {
    return moveContentsInLineResult;
  }
  EditorRawDOMPoint atUnnecessaryLineBreak(lastLineBreakContent);
  if (NS_WARN_IF(!atUnnecessaryLineBreak.IsSet())) {
    return Err(NS_ERROR_FAILURE);
  }
  // If the found unnecessary line break is not what we moved above, we
  // shouldn't remove it.  E.g., the web app may have inserted it intentionally.
  if (!movedContentRange.Contains(atUnnecessaryLineBreak)) {
    return moveContentsInLineResult;
  }

  AutoTransactionsConserveSelection dontChangeMySelection(*this);
  // If it's a text node and ending with a preformatted line break, we should
  // delete it.
  if (Text* textNode = Text::FromNode(lastLineBreakContent)) {
    MOZ_ASSERT(EditorUtils::IsNewLinePreformatted(*textNode));
    if (textNode->TextDataLength() > 1) {
      nsresult rv = DeleteTextWithTransaction(
          MOZ_KnownLive(*textNode), textNode->TextDataLength() - 1u, 1u);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DeleteTextWithTransaction() failed");
        return Err(rv);
      }
      return moveContentsInLineResult;
    }
  } else {
    MOZ_ASSERT(lastLineBreakContent->IsHTMLElement(nsGkAtoms::br));
  }
  // If last line break content is the only content of its inline parent, we
  // should remove the parent too.
  if (const RefPtr<Element> inlineElement =
          HTMLEditUtils::GetMostDistantAncestorEditableEmptyInlineElement(
              *lastLineBreakContent, &aEditingHost)) {
    nsresult rv = DeleteNodeWithTransaction(*inlineElement);
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return Err(rv);
    }
    return moveContentsInLineResult;
  }
  // Or if the text node has only the preformatted line break or <br> element,
  // we should remove it.
  nsresult rv = DeleteNodeWithTransaction(*lastLineBreakContent);
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
    return Err(rv);
  }
  return moveContentsInLineResult;
}

Result<bool, nsresult> HTMLEditor::CanMoveNodeOrChildren(
    const nsIContent& aContent, const nsINode& aNewContainer) const {
  if (HTMLEditUtils::CanNodeContain(aNewContainer, aContent)) {
    return true;
  }
  if (aContent.IsElement()) {
    return CanMoveChildren(*aContent.AsElement(), aNewContainer);
  }
  return true;
}

Result<MoveNodeResult, nsresult> HTMLEditor::MoveNodeOrChildrenWithTransaction(
    nsIContent& aContentToMove, const EditorDOMPoint& aPointToInsert,
    PreserveWhiteSpaceStyle aPreserveWhiteSpaceStyle) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointToInsert.IsInContentNode());

  const auto destWhiteSpaceStyle = [&]() -> Maybe<StyleWhiteSpace> {
    if (aPreserveWhiteSpaceStyle == PreserveWhiteSpaceStyle::No ||
        !aPointToInsert.IsInContentNode()) {
      return Nothing();
    }
    auto style = EditorUtils::GetComputedWhiteSpaceStyle(
        *aPointToInsert.ContainerAs<nsIContent>());
    if (NS_WARN_IF(style.isSome() &&
                   style.value() == StyleWhiteSpace::PreSpace)) {
      return Nothing();
    }
    return style;
  }();
  const auto srcWhiteSpaceStyle = [&]() -> Maybe<StyleWhiteSpace> {
    if (aPreserveWhiteSpaceStyle == PreserveWhiteSpaceStyle::No) {
      return Nothing();
    }
    auto style = EditorUtils::GetComputedWhiteSpaceStyle(aContentToMove);
    if (NS_WARN_IF(style.isSome() &&
                   style.value() == StyleWhiteSpace::PreSpace)) {
      return Nothing();
    }
    return style;
  }();
  const auto GetWhiteSpaceStyleValue = [](StyleWhiteSpace aStyleWhiteSpace) {
    switch (aStyleWhiteSpace) {
      case StyleWhiteSpace::Normal:
        return u"normal"_ns;
      case StyleWhiteSpace::Pre:
        return u"pre"_ns;
      case StyleWhiteSpace::Nowrap:
        return u"nowrap"_ns;
      case StyleWhiteSpace::PreWrap:
        return u"pre-wrap"_ns;
      case StyleWhiteSpace::PreLine:
        return u"pre-line"_ns;
      case StyleWhiteSpace::BreakSpaces:
        return u"break-spaces"_ns;
      case StyleWhiteSpace::PreSpace:
        MOZ_ASSERT_UNREACHABLE("Don't handle -moz-pre-space");
        return u""_ns;
      default:
        MOZ_ASSERT_UNREACHABLE("Handle the new white-space value");
        return u""_ns;
    }
  };

  // Check if this node can go into the destination node
  if (HTMLEditUtils::CanNodeContain(*aPointToInsert.GetContainer(),
                                    aContentToMove)) {
    EditorDOMPoint pointToInsert(aPointToInsert);
    // Preserve white-space in the new position with using `style` attribute.
    // This is additional path from point of view of our traditional behavior.
    // Therefore, ignore errors especially if we got unexpected DOM tree.
    if (destWhiteSpaceStyle.isSome() && srcWhiteSpaceStyle.isSome() &&
        destWhiteSpaceStyle.value() != srcWhiteSpaceStyle.value()) {
      // Set `white-space` with `style` attribute if it's nsStyledElement.
      if (nsStyledElement* styledElement =
              nsStyledElement::FromNode(&aContentToMove)) {
        DebugOnly<nsresult> rvIgnored =
            CSSEditUtils::SetCSSPropertyWithTransaction(
                *this, MOZ_KnownLive(*styledElement), *nsGkAtoms::white_space,
                GetWhiteSpaceStyleValue(srcWhiteSpaceStyle.value()));
        if (NS_WARN_IF(Destroyed())) {
          return Err(NS_ERROR_EDITOR_DESTROYED);
        }
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                             "CSSEditUtils::SetCSSPropertyWithTransaction("
                             "nsGkAtoms::white_space) failed, but ignored");
      }
      // Otherwise, if the dest container can have <span> element and <span>
      // element can have the moving content node, we should insert it.
      else if (HTMLEditUtils::CanNodeContain(*aPointToInsert.GetContainer(),
                                             *nsGkAtoms::span) &&
               HTMLEditUtils::CanNodeContain(*nsGkAtoms::span,
                                             aContentToMove)) {
        RefPtr<Element> newSpanElement = CreateHTMLContent(nsGkAtoms::span);
        if (NS_WARN_IF(!newSpanElement)) {
          return Err(NS_ERROR_FAILURE);
        }
        nsAutoString styleAttrValue(u"white-space: "_ns);
        styleAttrValue.Append(
            GetWhiteSpaceStyleValue(srcWhiteSpaceStyle.value()));
        IgnoredErrorResult error;
        newSpanElement->SetAttr(nsGkAtoms::style, styleAttrValue, error);
        NS_WARNING_ASSERTION(!error.Failed(),
                             "Element::SetAttr(nsGkAtoms::span) failed");
        if (MOZ_LIKELY(!error.Failed())) {
          Result<CreateElementResult, nsresult> insertSpanElementResult =
              InsertNodeWithTransaction<Element>(*newSpanElement,
                                                 aPointToInsert);
          if (MOZ_UNLIKELY(insertSpanElementResult.isErr())) {
            if (NS_WARN_IF(insertSpanElementResult.inspectErr() ==
                           NS_ERROR_EDITOR_DESTROYED)) {
              return Err(NS_ERROR_EDITOR_DESTROYED);
            }
            NS_WARNING(
                "HTMLEditor::InsertNodeWithTransaction() failed, but ignored");
          } else {
            // We should move the node into the new <span> to preserve the
            // style.
            pointToInsert.Set(newSpanElement, 0u);
            // We should put caret after aContentToMove after moving it so that
            // we do not need the suggested caret point here.
            insertSpanElementResult.inspect().IgnoreCaretPointSuggestion();
          }
        }
      }
    }
    // If it can, move it there.
    Result<MoveNodeResult, nsresult> moveNodeResult =
        MoveNodeWithTransaction(aContentToMove, pointToInsert);
    NS_WARNING_ASSERTION(moveNodeResult.isOk(),
                         "HTMLEditor::MoveNodeWithTransaction() failed");
    // XXX This is odd to override the handled state here, but stopping this
    //     hits an NS_ASSERTION in WhiteSpaceVisibilityKeeper::
    //     MergeFirstLineOfRightBlockElementIntoAncestorLeftBlockElement.
    if (moveNodeResult.isOk()) {
      MoveNodeResult unwrappedMoveNodeResult = moveNodeResult.unwrap();
      unwrappedMoveNodeResult.MarkAsHandled();
      return unwrappedMoveNodeResult;
    }
    return moveNodeResult;
  }

  // If it can't, move its children (if any), and then delete it.
  auto moveNodeResult =
      [&]() MOZ_CAN_RUN_SCRIPT -> Result<MoveNodeResult, nsresult> {
    if (!aContentToMove.IsElement()) {
      return MoveNodeResult::HandledResult(aPointToInsert);
    }
    Result<MoveNodeResult, nsresult> moveChildrenResult =
        MoveChildrenWithTransaction(MOZ_KnownLive(*aContentToMove.AsElement()),
                                    aPointToInsert, aPreserveWhiteSpaceStyle);
    NS_WARNING_ASSERTION(moveChildrenResult.isOk(),
                         "HTMLEditor::MoveChildrenWithTransaction() failed");
    return moveChildrenResult;
  }();
  if (MOZ_UNLIKELY(moveNodeResult.isErr())) {
    return moveNodeResult;  // Already warned in the lambda.
  }

  nsresult rv = DeleteNodeWithTransaction(aContentToMove);
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
    moveNodeResult.inspect().IgnoreCaretPointSuggestion();
    return Err(rv);
  }
  if (!MayHaveMutationEventListeners()) {
    return moveNodeResult;
  }
  // Mutation event listener may make `offset` value invalid with
  // removing some previous children while we call
  // `DeleteNodeWithTransaction()` so that we should adjust it here.
  if (moveNodeResult.inspect().NextInsertionPointRef().IsSetAndValid()) {
    return moveNodeResult;
  }
  moveNodeResult.inspect().IgnoreCaretPointSuggestion();
  return MoveNodeResult::HandledResult(
      EditorDOMPoint::AtEndOf(*aPointToInsert.GetContainer()));
}

Result<bool, nsresult> HTMLEditor::CanMoveChildren(
    const Element& aElement, const nsINode& aNewContainer) const {
  if (NS_WARN_IF(&aElement == &aNewContainer)) {
    return Err(NS_ERROR_FAILURE);
  }
  for (nsIContent* childContent = aElement.GetFirstChild(); childContent;
       childContent = childContent->GetNextSibling()) {
    Result<bool, nsresult> result =
        CanMoveNodeOrChildren(*childContent, aNewContainer);
    if (result.isErr() || result.inspect()) {
      return result;
    }
  }
  return false;
}

Result<MoveNodeResult, nsresult> HTMLEditor::MoveChildrenWithTransaction(
    Element& aElement, const EditorDOMPoint& aPointToInsert,
    PreserveWhiteSpaceStyle aPreserveWhiteSpaceStyle) {
  MOZ_ASSERT(aPointToInsert.IsSet());

  if (NS_WARN_IF(&aElement == aPointToInsert.GetContainer())) {
    return Err(NS_ERROR_INVALID_ARG);
  }

  MoveNodeResult moveChildrenResult =
      MoveNodeResult::IgnoredResult(aPointToInsert);
  while (aElement.GetFirstChild()) {
    Result<MoveNodeResult, nsresult> moveNodeOrChildrenResult =
        MoveNodeOrChildrenWithTransaction(
            MOZ_KnownLive(*aElement.GetFirstChild()),
            moveChildrenResult.NextInsertionPointRef(),
            aPreserveWhiteSpaceStyle);
    if (MOZ_UNLIKELY(moveNodeOrChildrenResult.isErr())) {
      NS_WARNING("HTMLEditor::MoveNodeOrChildrenWithTransaction() failed");
      return moveNodeOrChildrenResult;
    }
    moveChildrenResult |= moveNodeOrChildrenResult.inspect();
  }
  return moveChildrenResult;
}

void HTMLEditor::MoveAllChildren(nsINode& aContainer,
                                 const EditorRawDOMPoint& aPointToInsert,
                                 ErrorResult& aError) {
  MOZ_ASSERT(!aError.Failed());

  if (!aContainer.HasChildren()) {
    return;
  }
  nsIContent* firstChild = aContainer.GetFirstChild();
  if (NS_WARN_IF(!firstChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  nsIContent* lastChild = aContainer.GetLastChild();
  if (NS_WARN_IF(!lastChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  MoveChildrenBetween(*firstChild, *lastChild, aPointToInsert, aError);
  NS_WARNING_ASSERTION(!aError.Failed(),
                       "HTMLEditor::MoveChildrenBetween() failed");
}

void HTMLEditor::MoveChildrenBetween(nsIContent& aFirstChild,
                                     nsIContent& aLastChild,
                                     const EditorRawDOMPoint& aPointToInsert,
                                     ErrorResult& aError) {
  nsCOMPtr<nsINode> oldContainer = aFirstChild.GetParentNode();
  if (NS_WARN_IF(oldContainer != aLastChild.GetParentNode()) ||
      NS_WARN_IF(!aPointToInsert.IsInContentNode()) ||
      NS_WARN_IF(!aPointToInsert.CanContainerHaveChildren())) {
    aError.Throw(NS_ERROR_INVALID_ARG);
    return;
  }

  // First, store all children which should be moved to the new container.
  AutoTArray<nsCOMPtr<nsIContent>, 10> children;
  for (nsIContent* child = &aFirstChild; child;
       child = child->GetNextSibling()) {
    children.AppendElement(child);
    if (child == &aLastChild) {
      break;
    }
  }

  if (NS_WARN_IF(children.LastElement() != &aLastChild)) {
    aError.Throw(NS_ERROR_INVALID_ARG);
    return;
  }

  nsCOMPtr<nsIContent> newContainer = aPointToInsert.ContainerAs<nsIContent>();
  nsCOMPtr<nsIContent> nextNode = aPointToInsert.GetChild();
  for (size_t i = children.Length(); i > 0; --i) {
    nsCOMPtr<nsIContent>& child = children[i - 1];
    if (child->GetParentNode() != oldContainer) {
      // If the child has been moved to different container, we shouldn't
      // touch it.
      continue;
    }
    if (NS_WARN_IF(!HTMLEditUtils::IsRemovableNode(*child))) {
      aError.Throw(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      return;
    }
    oldContainer->RemoveChild(*child, aError);
    if (NS_WARN_IF(Destroyed())) {
      aError.Throw(NS_ERROR_EDITOR_DESTROYED);
      return;
    }
    if (aError.Failed()) {
      NS_WARNING("nsINode::RemoveChild() failed");
      return;
    }
    if (nextNode) {
      // If we're not appending the children to the new container, we should
      // check if referring next node of insertion point is still in the new
      // container.
      EditorRawDOMPoint pointToInsert(nextNode);
      if (NS_WARN_IF(!pointToInsert.IsSet()) ||
          NS_WARN_IF(pointToInsert.GetContainer() != newContainer)) {
        // The next node of insertion point has been moved by mutation observer.
        // Let's stop moving the remaining nodes.
        // XXX Or should we move remaining children after the last moved child?
        aError.Throw(NS_ERROR_FAILURE);
        return;
      }
    }
    if (NS_WARN_IF(
            !EditorUtils::IsEditableContent(*newContainer, EditorType::HTML))) {
      aError.Throw(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      return;
    }
    newContainer->InsertBefore(*child, nextNode, aError);
    if (NS_WARN_IF(Destroyed())) {
      aError.Throw(NS_ERROR_EDITOR_DESTROYED);
      return;
    }
    if (aError.Failed()) {
      NS_WARNING("nsINode::InsertBefore() failed");
      return;
    }
    // If the child was inserted or appended properly, the following children
    // should be inserted before it.  Otherwise, keep using current position.
    if (child->GetParentNode() == newContainer) {
      nextNode = child;
    }
  }
}

void HTMLEditor::MovePreviousSiblings(nsIContent& aChild,
                                      const EditorRawDOMPoint& aPointToInsert,
                                      ErrorResult& aError) {
  MOZ_ASSERT(!aError.Failed());

  if (NS_WARN_IF(!aChild.GetParentNode())) {
    aError.Throw(NS_ERROR_INVALID_ARG);
    return;
  }
  nsIContent* firstChild = aChild.GetParentNode()->GetFirstChild();
  if (NS_WARN_IF(!firstChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  nsIContent* lastChild =
      &aChild == firstChild ? firstChild : aChild.GetPreviousSibling();
  if (NS_WARN_IF(!lastChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  MoveChildrenBetween(*firstChild, *lastChild, aPointToInsert, aError);
  NS_WARNING_ASSERTION(!aError.Failed(),
                       "HTMLEditor::MoveChildrenBetween() failed");
}

void HTMLEditor::MoveInclusiveNextSiblings(
    nsIContent& aChild, const EditorRawDOMPoint& aPointToInsert,
    ErrorResult& aError) {
  MOZ_ASSERT(!aError.Failed());

  if (NS_WARN_IF(!aChild.GetParentNode())) {
    aError.Throw(NS_ERROR_INVALID_ARG);
    return;
  }
  nsIContent* lastChild = aChild.GetParentNode()->GetLastChild();
  if (NS_WARN_IF(!lastChild)) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }
  MoveChildrenBetween(aChild, *lastChild, aPointToInsert, aError);
  NS_WARNING_ASSERTION(!aError.Failed(),
                       "HTMLEditor::MoveChildrenBetween() failed");
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoBlockElementsJoiner::
    DeleteContentButKeepTableStructure(HTMLEditor& aHTMLEditor,
                                       nsIContent& aContent) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  if (!HTMLEditUtils::IsAnyTableElementButNotTable(&aContent)) {
    nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(aContent);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "EditorBase::DeleteNodeWithTransaction() failed");
    return rv;
  }

  // XXX For performance, this should just call
  //     DeleteContentButKeepTableStructure() while there are children in
  //     aContent.  If we need to avoid infinite loop because mutation event
  //     listeners can add unexpected nodes into aContent, we should just loop
  //     only original count of the children.
  AutoTArray<OwningNonNull<nsIContent>, 10> childList;
  for (nsIContent* child = aContent.GetFirstChild(); child;
       child = child->GetNextSibling()) {
    childList.AppendElement(*child);
  }

  for (const auto& child : childList) {
    // MOZ_KnownLive because 'childList' is guaranteed to
    // keep it alive.
    nsresult rv =
        DeleteContentButKeepTableStructure(aHTMLEditor, MOZ_KnownLive(child));
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::DeleteContentButKeepTableStructure() failed");
      return rv;
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::DeleteMostAncestorMailCiteElementIfEmpty(
    nsIContent& aContent) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // The element must be `<blockquote type="cite">` or
  // `<span _moz_quote="true">`.
  RefPtr<Element> mailCiteElement =
      GetMostDistantAncestorMailCiteElement(aContent);
  if (!mailCiteElement) {
    return NS_OK;
  }
  bool seenBR = false;
  if (!HTMLEditUtils::IsEmptyNode(*mailCiteElement,
                                  {EmptyCheckOption::TreatListItemAsVisible,
                                   EmptyCheckOption::TreatTableCellAsVisible},
                                  &seenBR)) {
    return NS_OK;
  }
  EditorDOMPoint atEmptyMailCiteElement(mailCiteElement);
  {
    AutoEditorDOMPointChildInvalidator lockOffset(atEmptyMailCiteElement);
    nsresult rv = DeleteNodeWithTransaction(*mailCiteElement);
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }

  if (!atEmptyMailCiteElement.IsSet() || !seenBR) {
    NS_WARNING_ASSERTION(
        atEmptyMailCiteElement.IsSet(),
        "Mutation event listener might changed the DOM tree during "
        "EditorBase::DeleteNodeWithTransaction(), but ignored");
    return NS_OK;
  }

  Result<CreateElementResult, nsresult> insertBRElementResult =
      InsertBRElement(WithTransaction::Yes, atEmptyMailCiteElement);
  if (MOZ_UNLIKELY(insertBRElementResult.isErr())) {
    NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
    return insertBRElementResult.unwrapErr();
  }
  MOZ_ASSERT(insertBRElementResult.inspect().GetNewNode());
  insertBRElementResult.inspect().IgnoreCaretPointSuggestion();
  nsresult rv = CollapseSelectionTo(
      EditorRawDOMPoint(insertBRElementResult.inspect().GetNewNode()));
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "EditorBase::::CollapseSelectionTo() failed, but ignored");
  return NS_OK;
}

Element* HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::
    ScanEmptyBlockInclusiveAncestor(const HTMLEditor& aHTMLEditor,
                                    nsIContent& aStartContent) {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!mEmptyInclusiveAncestorBlockElement);

  // If we are inside an empty block, delete it.
  // Note: do NOT delete table elements this way.
  // Note: do NOT delete non-editable block element.
  Element* editableBlockElement = HTMLEditUtils::GetInclusiveAncestorElement(
      aStartContent, HTMLEditUtils::ClosestEditableBlockElement);
  if (!editableBlockElement) {
    return nullptr;
  }
  // XXX Perhaps, this is slow loop.  If empty blocks are nested, then,
  //     each block checks whether it's empty or not.  However, descendant
  //     blocks are checked again and again by IsEmptyNode().  Perhaps, it
  //     should be able to take "known empty element" for avoiding same checks.
  while (editableBlockElement &&
         HTMLEditUtils::IsRemovableFromParentNode(*editableBlockElement) &&
         !HTMLEditUtils::IsAnyTableElement(editableBlockElement) &&
         HTMLEditUtils::IsEmptyNode(*editableBlockElement)) {
    mEmptyInclusiveAncestorBlockElement = editableBlockElement;
    editableBlockElement = HTMLEditUtils::GetAncestorElement(
        *mEmptyInclusiveAncestorBlockElement,
        HTMLEditUtils::ClosestEditableBlockElement);
  }
  if (!mEmptyInclusiveAncestorBlockElement) {
    return nullptr;
  }

  // XXX Because of not checking whether found block element is editable
  //     in the above loop, empty ediable block element may be overwritten
  //     with empty non-editable clock element.  Therefore, we fail to
  //     remove the found empty nodes.
  if (NS_WARN_IF(!mEmptyInclusiveAncestorBlockElement->IsEditable()) ||
      NS_WARN_IF(!mEmptyInclusiveAncestorBlockElement->GetParentElement())) {
    mEmptyInclusiveAncestorBlockElement = nullptr;
  }
  return mEmptyInclusiveAncestorBlockElement;
}

nsresult HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::
    ComputeTargetRanges(const HTMLEditor& aHTMLEditor,
                        nsIEditor::EDirection aDirectionAndAmount,
                        const Element& aEditingHost,
                        AutoRangeArray& aRangesToDelete) const {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);

  // We'll delete `mEmptyInclusiveAncestorBlockElement` node from the tree, but
  // we should return the range from start/end of next/previous editable content
  // to end/start of the element for compatiblity with the other browsers.
  switch (aDirectionAndAmount) {
    case nsIEditor::eNone:
      break;
    case nsIEditor::ePrevious:
    case nsIEditor::ePreviousWord:
    case nsIEditor::eToBeginningOfLine: {
      EditorRawDOMPoint startPoint =
          HTMLEditUtils::GetPreviousEditablePoint<EditorRawDOMPoint>(
              *mEmptyInclusiveAncestorBlockElement, &aEditingHost,
              // In this case, we don't join block elements so that we won't
              // delete invisible trailing whitespaces in the previous element.
              InvisibleWhiteSpaces::Preserve,
              // In this case, we won't join table cells so that we should
              // get a range which is in a table cell even if it's in a
              // table.
              TableBoundary::NoCrossAnyTableElement);
      if (!startPoint.IsSet()) {
        NS_WARNING(
            "HTMLEditUtils::GetPreviousEditablePoint() didn't return a valid "
            "point");
        return NS_ERROR_FAILURE;
      }
      nsresult rv = aRangesToDelete.SetStartAndEnd(
          startPoint,
          EditorRawDOMPoint::AtEndOf(mEmptyInclusiveAncestorBlockElement));
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "AutoRangeArray::SetStartAndEnd() failed");
      return rv;
    }
    case nsIEditor::eNext:
    case nsIEditor::eNextWord:
    case nsIEditor::eToEndOfLine: {
      EditorRawDOMPoint endPoint =
          HTMLEditUtils::GetNextEditablePoint<EditorRawDOMPoint>(
              *mEmptyInclusiveAncestorBlockElement, &aEditingHost,
              // In this case, we don't join block elements so that we won't
              // delete invisible trailing whitespaces in the next element.
              InvisibleWhiteSpaces::Preserve,
              // In this case, we won't join table cells so that we should
              // get a range which is in a table cell even if it's in a
              // table.
              TableBoundary::NoCrossAnyTableElement);
      if (!endPoint.IsSet()) {
        NS_WARNING(
            "HTMLEditUtils::GetNextEditablePoint() didn't return a valid "
            "point");
        return NS_ERROR_FAILURE;
      }
      nsresult rv = aRangesToDelete.SetStartAndEnd(
          EditorRawDOMPoint(mEmptyInclusiveAncestorBlockElement, 0), endPoint);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "AutoRangeArray::SetStartAndEnd() failed");
      return rv;
    }
    default:
      MOZ_ASSERT_UNREACHABLE("Handle the nsIEditor::EDirection value");
      break;
  }
  // No direction, let's select the element to be deleted.
  nsresult rv =
      aRangesToDelete.SelectNode(*mEmptyInclusiveAncestorBlockElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::SelectNode() failed");
  return rv;
}

Result<RefPtr<Element>, nsresult>
HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::
    MaybeInsertBRElementBeforeEmptyListItemElement(HTMLEditor& aHTMLEditor) {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement->GetParentElement());
  MOZ_ASSERT(HTMLEditUtils::IsListItem(mEmptyInclusiveAncestorBlockElement));

  // If the found empty block is a list item element and its grand parent
  // (i.e., parent of list element) is NOT a list element, insert <br>
  // element before the list element which has the empty list item.
  // This odd list structure may occur if `Document.execCommand("indent")`
  // is performed for list items.
  // XXX Chrome does not remove empty list elements when last content in
  //     last list item is deleted.  We should follow it since current
  //     behavior is annoying when you type new list item with selecting
  //     all list items.
  if (!HTMLEditUtils::IsFirstChild(*mEmptyInclusiveAncestorBlockElement,
                                   {WalkTreeOption::IgnoreNonEditableNode})) {
    return RefPtr<Element>();
  }

  EditorDOMPoint atParentOfEmptyListItem(
      mEmptyInclusiveAncestorBlockElement->GetParentElement());
  if (NS_WARN_IF(!atParentOfEmptyListItem.IsSet())) {
    return Err(NS_ERROR_FAILURE);
  }
  if (HTMLEditUtils::IsAnyListElement(atParentOfEmptyListItem.GetContainer())) {
    return RefPtr<Element>();
  }
  Result<CreateElementResult, nsresult> insertBRElementResult =
      aHTMLEditor.InsertBRElement(WithTransaction::Yes,
                                  atParentOfEmptyListItem);
  if (MOZ_UNLIKELY(insertBRElementResult.isErr())) {
    NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
    return insertBRElementResult.propagateErr();
  }
  CreateElementResult unwrappedInsertBRElementResult =
      insertBRElementResult.unwrap();
  nsresult rv = unwrappedInsertBRElementResult.SuggestCaretPointTo(
      aHTMLEditor, {SuggestCaret::OnlyIfHasSuggestion,
                    SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                    SuggestCaret::AndIgnoreTrivialError});
  if (NS_FAILED(rv)) {
    NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
    return Err(rv);
  }
  MOZ_ASSERT(unwrappedInsertBRElementResult.GetNewNode());
  return unwrappedInsertBRElementResult.UnwrapNewNode();
}

Result<EditorDOMPoint, nsresult> HTMLEditor::AutoDeleteRangesHandler::
    AutoEmptyBlockAncestorDeleter::GetNewCaretPoisition(
        const HTMLEditor& aHTMLEditor,
        nsIEditor::EDirection aDirectionAndAmount) const {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement->GetParentElement());
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  switch (aDirectionAndAmount) {
    case nsIEditor::eNext:
    case nsIEditor::eNextWord:
    case nsIEditor::eToEndOfLine: {
      // Collapse Selection to next node of after empty block element
      // if there is.  Otherwise, to just after the empty block.
      auto afterEmptyBlock(
          EditorDOMPoint::After(mEmptyInclusiveAncestorBlockElement));
      MOZ_ASSERT(afterEmptyBlock.IsSet());
      if (nsIContent* nextContentOfEmptyBlock = HTMLEditUtils::GetNextContent(
              afterEmptyBlock, {}, aHTMLEditor.ComputeEditingHost())) {
        EditorDOMPoint pt = HTMLEditUtils::GetGoodCaretPointFor<EditorDOMPoint>(
            *nextContentOfEmptyBlock, aDirectionAndAmount);
        if (!pt.IsSet()) {
          NS_WARNING("HTMLEditUtils::GetGoodCaretPointFor() failed");
          return Err(NS_ERROR_FAILURE);
        }
        return pt;
      }
      if (NS_WARN_IF(!afterEmptyBlock.IsSet())) {
        return Err(NS_ERROR_FAILURE);
      }
      return afterEmptyBlock;
    }
    case nsIEditor::ePrevious:
    case nsIEditor::ePreviousWord:
    case nsIEditor::eToBeginningOfLine: {
      // Collapse Selection to previous editable node of the empty block
      // if there is.  Otherwise, to after the empty block.
      EditorRawDOMPoint atEmptyBlock(mEmptyInclusiveAncestorBlockElement);
      if (nsIContent* previousContentOfEmptyBlock =
              HTMLEditUtils::GetPreviousContent(
                  atEmptyBlock, {WalkTreeOption::IgnoreNonEditableNode},
                  aHTMLEditor.ComputeEditingHost())) {
        EditorDOMPoint pt = HTMLEditUtils::GetGoodCaretPointFor<EditorDOMPoint>(
            *previousContentOfEmptyBlock, aDirectionAndAmount);
        if (!pt.IsSet()) {
          NS_WARNING("HTMLEditUtils::GetGoodCaretPointFor() failed");
          return Err(NS_ERROR_FAILURE);
        }
        return pt;
      }
      auto afterEmptyBlock =
          EditorDOMPoint::After(*mEmptyInclusiveAncestorBlockElement);
      if (NS_WARN_IF(!afterEmptyBlock.IsSet())) {
        return Err(NS_ERROR_FAILURE);
      }
      return afterEmptyBlock;
    }
    case nsIEditor::eNone:
      return EditorDOMPoint();
    default:
      MOZ_CRASH(
          "AutoEmptyBlockAncestorDeleter doesn't support this action yet");
      return EditorDOMPoint();
  }
}

Result<EditActionResult, nsresult>
HTMLEditor::AutoDeleteRangesHandler::AutoEmptyBlockAncestorDeleter::Run(
    HTMLEditor& aHTMLEditor, nsIEditor::EDirection aDirectionAndAmount) {
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement);
  MOZ_ASSERT(mEmptyInclusiveAncestorBlockElement->GetParentElement());
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());

  if (HTMLEditUtils::IsListItem(mEmptyInclusiveAncestorBlockElement)) {
    Result<RefPtr<Element>, nsresult> result =
        MaybeInsertBRElementBeforeEmptyListItemElement(aHTMLEditor);
    if (MOZ_UNLIKELY(result.isErr())) {
      NS_WARNING(
          "AutoEmptyBlockAncestorDeleter::"
          "MaybeInsertBRElementBeforeEmptyListItemElement() failed");
      return result.propagateErr();
    }
    // If a `<br>` element is inserted, caret should be moved to after it.
    if (RefPtr<Element> brElement = result.unwrap()) {
      nsresult rv =
          aHTMLEditor.CollapseSelectionTo(EditorRawDOMPoint(brElement));
      if (NS_FAILED(rv)) {
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "EditorBase::CollapseSelectionTo() failed");
        return Err(rv);
      }
    }
  } else {
    Result<EditorDOMPoint, nsresult> result =
        GetNewCaretPoisition(aHTMLEditor, aDirectionAndAmount);
    if (result.isErr()) {
      NS_WARNING(
          "AutoEmptyBlockAncestorDeleter::GetNewCaretPoisition() failed");
      return result.propagateErr();
    }
    if (result.inspect().IsSet()) {
      nsresult rv = aHTMLEditor.CollapseSelectionTo(result.inspect());
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::CollapseSelectionTo() failed");
        return Err(rv);
      }
    }
  }
  nsresult rv = aHTMLEditor.DeleteNodeWithTransaction(
      MOZ_KnownLive(*mEmptyInclusiveAncestorBlockElement));
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
    return Err(rv);
  }
  return EditActionResult::HandledResult();
}

template <typename EditorDOMRangeType>
Result<EditorRawDOMRange, nsresult>
HTMLEditor::AutoDeleteRangesHandler::ExtendOrShrinkRangeToDelete(
    const HTMLEditor& aHTMLEditor, const nsFrameSelection* aFrameSelection,
    const EditorDOMRangeType& aRangeToDelete) const {
  MOZ_ASSERT(aHTMLEditor.IsEditActionDataAvailable());
  MOZ_ASSERT(!aRangeToDelete.Collapsed());
  MOZ_ASSERT(aRangeToDelete.IsPositioned());

  const nsIContent* commonAncestor = nsIContent::FromNodeOrNull(
      nsContentUtils::GetClosestCommonInclusiveAncestor(
          aRangeToDelete.StartRef().GetContainer(),
          aRangeToDelete.EndRef().GetContainer()));
  if (MOZ_UNLIKELY(NS_WARN_IF(!commonAncestor))) {
    return Err(NS_ERROR_FAILURE);
  }

  // Look for the common ancestor's block element.  It's fine that we get
  // non-editable block element which is ancestor of inline editing host
  // because the following code checks editing host too.
  const Element* const maybeNonEditableBlockElement =
      HTMLEditUtils::GetInclusiveAncestorElement(
          *commonAncestor, HTMLEditUtils::ClosestBlockElement);
  if (NS_WARN_IF(!maybeNonEditableBlockElement)) {
    return Err(NS_ERROR_FAILURE);
  }

  // Set up for loops and cache our root element
  RefPtr<Element> editingHost = aHTMLEditor.ComputeEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return Err(NS_ERROR_FAILURE);
  }

  // If only one list element is selected, and if the list element is empty,
  // we should delete only the list element.  Or if the list element is not
  // empty, we should make the list has only one empty list item element.
  if (const Element* maybeListElement =
          HTMLEditUtils::GetElementIfOnlyOneSelected(aRangeToDelete)) {
    if (HTMLEditUtils::IsAnyListElement(maybeListElement) &&
        !HTMLEditUtils::IsEmptyNode(*maybeListElement)) {
      EditorRawDOMRange range =
          HTMLEditUtils::GetRangeSelectingAllContentInAllListItems<
              EditorRawDOMRange>(*maybeListElement);
      if (range.IsPositioned()) {
        if (EditorUtils::IsEditableContent(
                *range.StartRef().ContainerAs<nsIContent>(),
                EditorType::HTML) &&
            EditorUtils::IsEditableContent(
                *range.EndRef().ContainerAs<nsIContent>(), EditorType::HTML)) {
          return range;
        }
      }
      // If the first and/or last list item is not editable, we need to do more
      // complicated things probably, but we just delete the list element with
      // invisible things around it for now since it must be rare case.
    }
    // Otherwise, if the list item is empty, we should delete it with invisible
    // things around it.
  }

  // Find previous visible things before start of selection
  EditorRawDOMRange rangeToDelete(aRangeToDelete);
  if (rangeToDelete.StartRef().GetContainer() != maybeNonEditableBlockElement &&
      rangeToDelete.StartRef().GetContainer() != editingHost) {
    for (;;) {
      WSScanResult backwardScanFromStartResult =
          WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundary(
              editingHost, rangeToDelete.StartRef());
      if (!backwardScanFromStartResult.ReachedCurrentBlockBoundary()) {
        break;
      }
      MOZ_ASSERT(backwardScanFromStartResult.GetContent() ==
                 WSRunScanner(editingHost, rangeToDelete.StartRef())
                     .GetStartReasonContent());
      // We want to keep looking up.  But stop if we are crossing table
      // element boundaries, or if we hit the root.
      if (HTMLEditUtils::IsAnyTableElement(
              backwardScanFromStartResult.GetContent()) ||
          backwardScanFromStartResult.GetContent() ==
              maybeNonEditableBlockElement ||
          backwardScanFromStartResult.GetContent() == editingHost) {
        break;
      }
      rangeToDelete.SetStart(
          backwardScanFromStartResult.PointAtContent<EditorRawDOMPoint>());
    }
    if (aFrameSelection && !aFrameSelection->IsValidSelectionPoint(
                               rangeToDelete.StartRef().GetContainer())) {
      NS_WARNING("Computed start container was out of selection limiter");
      return Err(NS_ERROR_FAILURE);
    }
  }

  // Expand selection endpoint only if we don't pass an invisible `<br>`, or if
  // we really needed to pass that `<br>` (i.e., its block is now totally
  // selected).

  // Find next visible things after end of selection
  EditorDOMPoint atFirstInvisibleBRElement;
  if (rangeToDelete.EndRef().GetContainer() != maybeNonEditableBlockElement &&
      rangeToDelete.EndRef().GetContainer() != editingHost) {
    for (;;) {
      WSRunScanner wsScannerAtEnd(editingHost, rangeToDelete.EndRef());
      WSScanResult forwardScanFromEndResult =
          wsScannerAtEnd.ScanNextVisibleNodeOrBlockBoundaryFrom(
              rangeToDelete.EndRef());
      if (forwardScanFromEndResult.ReachedBRElement()) {
        // XXX In my understanding, this is odd.  The end reason may not be
        //     same as the reached <br> element because the equality is
        //     guaranteed only when ReachedCurrentBlockBoundary() returns true.
        //     However, looks like that this code assumes that
        //     GetEndReasonContent() returns the (or a) <br> element.
        NS_ASSERTION(wsScannerAtEnd.GetEndReasonContent() ==
                         forwardScanFromEndResult.BRElementPtr(),
                     "End reason is not the reached <br> element");
        if (HTMLEditUtils::IsVisibleBRElement(
                *wsScannerAtEnd.GetEndReasonContent())) {
          break;
        }
        if (!atFirstInvisibleBRElement.IsSet()) {
          atFirstInvisibleBRElement =
              rangeToDelete.EndRef().To<EditorDOMPoint>();
        }
        rangeToDelete.SetEnd(
            EditorRawDOMPoint::After(*wsScannerAtEnd.GetEndReasonContent()));
        continue;
      }

      if (forwardScanFromEndResult.ReachedCurrentBlockBoundary()) {
        MOZ_ASSERT(forwardScanFromEndResult.GetContent() ==
                   wsScannerAtEnd.GetEndReasonContent());
        // We want to keep looking up.  But stop if we are crossing table
        // element boundaries, or if we hit the root.
        if (HTMLEditUtils::IsAnyTableElement(
                forwardScanFromEndResult.GetContent()) ||
            forwardScanFromEndResult.GetContent() ==
                maybeNonEditableBlockElement ||
            forwardScanFromEndResult.GetContent() == editingHost) {
          break;
        }
        rangeToDelete.SetEnd(
            forwardScanFromEndResult.PointAfterContent<EditorRawDOMPoint>());
        continue;
      }

      break;
    }

    if (aFrameSelection && !aFrameSelection->IsValidSelectionPoint(
                               rangeToDelete.EndRef().GetContainer())) {
      NS_WARNING("Computed end container was out of selection limiter");
      return Err(NS_ERROR_FAILURE);
    }
  }

  // If now, we select only the closest common ancestor list element or selects
  // all list items in it and it's not empty, we should make it have only one
  // list item which is empty.
  Element* selectedListElement =
      HTMLEditUtils::GetElementIfOnlyOneSelected(rangeToDelete);
  if (!selectedListElement ||
      !HTMLEditUtils::IsAnyListElement(selectedListElement)) {
    if (rangeToDelete.IsInContentNodes() && rangeToDelete.InSameContainer() &&
        HTMLEditUtils::IsAnyListElement(
            rangeToDelete.StartRef().ContainerAs<nsIContent>()) &&
        rangeToDelete.StartRef().IsStartOfContainer() &&
        rangeToDelete.EndRef().IsEndOfContainer()) {
      selectedListElement = rangeToDelete.StartRef().ContainerAs<Element>();
    } else {
      selectedListElement = nullptr;
    }
  }
  if (selectedListElement &&
      !HTMLEditUtils::IsEmptyNode(*selectedListElement)) {
    EditorRawDOMRange range =
        HTMLEditUtils::GetRangeSelectingAllContentInAllListItems<
            EditorRawDOMRange>(*selectedListElement);
    if (range.IsPositioned()) {
      if (EditorUtils::IsEditableContent(
              *range.StartRef().ContainerAs<nsIContent>(), EditorType::HTML) &&
          EditorUtils::IsEditableContent(
              *range.EndRef().ContainerAs<nsIContent>(), EditorType::HTML)) {
        return range;
      }
    }
  }

  if (atFirstInvisibleBRElement.IsInContentNode()) {
    // Find block node containing invisible `<br>` element.
    if (const RefPtr<const Element> editableBlockContainingBRElement =
            HTMLEditUtils::GetInclusiveAncestorElement(
                *atFirstInvisibleBRElement.ContainerAs<nsIContent>(),
                HTMLEditUtils::ClosestEditableBlockElement)) {
      if (rangeToDelete.Contains(
              EditorRawDOMPoint(editableBlockContainingBRElement))) {
        return rangeToDelete;
      }
      // Otherwise, the new range should end at the invisible `<br>`.
      if (aFrameSelection && !aFrameSelection->IsValidSelectionPoint(
                                 atFirstInvisibleBRElement.GetContainer())) {
        NS_WARNING(
            "Computed end container (`<br>` element) was out of selection "
            "limiter");
        return Err(NS_ERROR_FAILURE);
      }
      rangeToDelete.SetEnd(atFirstInvisibleBRElement);
    }
  }

  return rangeToDelete;
}

}  // namespace mozilla
